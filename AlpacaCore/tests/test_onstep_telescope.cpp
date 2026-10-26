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

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "sidereal_time_check.h"
#ifndef _WIN32
#include "concurrency_stress.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"
#endif

using alpacacore::DeviceType;

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

std::unique_ptr<alpacacore::TelescopeDriver> make_driver(int device_number) {
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    return alpacacore::vendor::onstep::create_onstep_telescope(device_number, conn);
}

}  // namespace

TEST_CASE("OnStep Telescope Driver - Defaults", "[onstep][telescope][unit]") {
    auto driver = make_driver(0);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == DeviceType::Telescope);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "OnStep Telescope");

    CHECK(driver->get_can_slew());
    CHECK(driver->get_can_slew_async());
    CHECK(driver->get_can_slew_alt_az());
    CHECK(driver->get_can_slew_alt_az_async());
    CHECK(driver->get_can_sync());
    CHECK_FALSE(driver->get_can_sync_alt_az());
    CHECK(driver->get_can_find_home());
    CHECK(driver->get_can_park());
    CHECK(driver->get_can_unpark());
    CHECK(driver->get_can_set_park());
    CHECK(driver->get_can_pulse_guide());
    CHECK_FALSE(driver->get_can_set_guide_rates());
    CHECK_FALSE(driver->get_can_set_pier_side());
    CHECK_FALSE(driver->get_can_set_declination_rate());
    CHECK_FALSE(driver->get_can_set_right_ascension_rate());
    CHECK(driver->get_can_set_tracking());
    CHECK(driver->get_alignment_mode() == alpacacore::AlignmentMode::GermanPolar);
}

TEST_CASE("OnStep Telescope Driver - Device metadata", "[onstep][telescope][unit]") {
    auto driver = make_driver(3);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "OnStep LX200-Protocol Telescope Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore OnStep Driver v0.1");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "OnStep_3");
}

TEST_CASE("OnStep Telescope Driver - Not connected throws", "[onstep][telescope][unit]") {
    auto driver = make_driver(0);

    require_alpaca_error([&]() { (void)driver->get_right_ascension(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_declination(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_altitude(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_azimuth(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_tracking(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->slew_to_target_async(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { driver->slew_to_coordinates_async(1.0, 1.0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("OnStep Telescope Driver - Unsupported actions", "[onstep][telescope][unit]") {
    auto driver = make_driver(0);

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("OnStep Telescope Driver - Device-specific behavior", "[onstep][telescope][unit]") {
    auto driver = make_driver(0);

    // Target coordinate persistence: independent RA/Dec storage, readable
    // without a live connection (matches iOptron/SynScan convention).
    driver->set_target_right_ascension(12.5);
    driver->set_target_declination(-30.0);
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 12.5);
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), -30.0);

    driver->set_target_right_ascension(1.25);
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 1.25);
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), -30.0);  // unaffected by the RA update

    // Axis rate ranges: primary/secondary valid, tertiary unsupported.
    auto primary = driver->get_axis_rate_range(0);
    CHECK(primary.second >= primary.first);
    auto secondary = driver->get_axis_rate_range(1);
    CHECK(secondary.second >= secondary.first);
    CHECK(driver->get_axis_rate_ranges(2).empty());
    auto tertiary = driver->get_axis_rate_range(2);
    CHECK(tertiary.first == 0.0);
    CHECK(tertiary.second == 0.0);

    // Telescope-wide static properties.
    CHECK(driver->get_tracking_rates() == std::vector<int>{0});
    CHECK(driver->get_slew_settle_time() >= 0);
    CHECK(driver->get_equatorial_system() == alpacacore::EquatorialSystem::Topocentric);
}

TEST_CASE("OnStep Telescope Driver - Value range validation", "[onstep][telescope][unit]") {
    auto driver = make_driver(0);

    require_alpaca_error([&]() { driver->set_target_right_ascension(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_right_ascension(24.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_elevation(-300.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_elevation(10000.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_latitude(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_latitude(90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_longitude(-180.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_longitude(180.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_slew_settle_time(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_aperture_diameter(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_focal_length(-0.1); }, alpacacore::AlpacaError::InvalidValue);

    // Out-of-range axis raises InvalidValue from AxisRates even while disconnected (#516);
    // the same rule for CanMoveAxis is pinned for every telescope by the contract sweep.
    require_alpaca_error([&]() { (void)driver->get_axis_rate_ranges(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { (void)driver->get_axis_rate_ranges(3); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("OnStep Telescope Driver - State machine", "[onstep][telescope][unit]") {
    auto driver = make_driver(0);

    // Nothing is knowable about live mount state without a connection, so
    // Slewing/IsPulseGuiding must throw NotConnected rather than report a
    // stale/default value that could mislead a client into skipping Connect.
    require_alpaca_error([&]() { (void)driver->get_slewing(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_is_pulse_guiding(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_at_home(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_at_park(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("OnStep Telescope Driver - Unsupported methods", "[onstep][telescope][unit]") {
    auto driver = make_driver(0);

    // Guide rate and pier side are fixed/read-only on this driver; the
    // setters must report PropertyNotImplemented, not a generic driver error
    // or a silent no-op (ConformU distinguishes "not implemented" from
    // "driver error").
    require_alpaca_error([&]() { driver->set_guide_rate({0.001, 0.001}); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&]() { driver->set_side_of_pier(0); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&]() { driver->set_declination_rate(1.0); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&]() { driver->set_right_ascension_rate(1.0); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&]() { driver->sync_to_alt_az(45.0, 90.0); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->set_tracking_rate(1); }, alpacacore::AlpacaError::PropertyNotImplemented);
}

// open-astro#346, the shape #304 fixed on the Sky-Watcher driver: ASCOM treats
// TargetRightAscension and TargetDeclination as independent properties, each
// throwing ValueNotSet until that property itself has been written. One shared
// flag let a write to either unlock both, so a client reading the target it
// did not set got a default 0 rather than an error -- which ConformU reports as
// "Read before write should generate an error and didn't".
TEST_CASE("OnStep Telescope Driver - the two target properties are independent", "[onstep][telescope][unit]") {
    auto driver = make_driver(0);

    // Neither written yet: both refuse.
    require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    // RA alone unlocks RA alone. This is the assertion that fails on one
    // shared flag: Dec used to read back 0.0 here.
    driver->set_target_right_ascension(7.25);
    CHECK(driver->get_target_right_ascension() == 7.25);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    // SlewToTarget, SlewToTargetAsync and SyncToTarget still need BOTH halves:
    // a half-set pair must refuse rather than slew to a default Dec. These
    // guards run before the connection check, so they hold on a disconnected
    // driver.
    require_alpaca_error([&]() { driver->slew_to_target(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { driver->slew_to_target_async(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { driver->sync_to_target(); }, alpacacore::AlpacaError::ValueNotSet);

    // Once Dec is written too, both read back.
    driver->set_target_declination(-30.25);
    CHECK(driver->get_target_right_ascension() == 7.25);
    CHECK(driver->get_target_declination() == -30.25);
}

#ifndef _WIN32

// ── The client-clock disagreement warning (#409) ─────────────────────────────

TEST_CASE("OnStep Telescope Driver - a far-off client UTCDate is logged once per connection on a disciplined host",
          "[onstep][telescope][unit]") {
    // OnStep keeps its own clock and UTCDate writes it, so the driver keeps
    // aiming by the client's instant (the mount does too); what it adds is
    // the shared once-per-connection WARN when the host is NTP-disciplined
    // and the client disagrees by more than the router's threshold. The
    // discipline probe is forced so the case is deterministic on a CI
    // runner with no NTP, and the sink is restored by the guard.
    struct ProbeGuard {
        ProbeGuard() {
            alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return true; });
        }
        ~ProbeGuard() { alpacacore::util::ClientUtcWarning::set_host_synchronized_probe(nullptr); }
    } probe_guard;
    std::atomic<int> warns{0};
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Warn && component == "OnStep" &&
                message.find("Client UTCDate disagrees") != std::string::npos) {
                ++warns;
            }
        });

    // :SL, :SC and :SG are the three writes set_time() makes; the wrapper
    // wants a bare "1" for the time and the offset and anything non-empty
    // for the date. The canned "0#" default would make the mount reject the
    // time and the driver throw before it ever cached the instant.
    alpacacore::test::FakeMountServer server([](const std::string& chunk) {
        if (chunk.rfind(":SL", 0) == 0 || chunk.rfind(":SC", 0) == 0 || chunk.rfind(":SG", 0) == 0) {
            return std::string("1");
        }
        if (chunk.size() >= 2 && chunk[0] == 'K') {
            return std::string(1, chunk[1]) + "#";
        }
        return std::string("0#");
    });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 50;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    const auto far = std::chrono::system_clock::now() + std::chrono::minutes(30);
    driver->set_utc_date(std::chrono::system_clock::now());  // agrees: no line, budget untouched
    CHECK(warns.load() == 0);
    driver->set_utc_date(far);
    CHECK(warns.load() == 1);
    driver->set_utc_date(far + std::chrono::seconds(1));
    driver->set_utc_date(far + std::chrono::seconds(2));
    CHECK(warns.load() == 1);

    // The readback still honours the client: it is the client's property.
    const auto readback = driver->get_utc_date();
    const auto delta = std::chrono::duration_cast<std::chrono::seconds>(readback - far);
    CHECK(delta >= std::chrono::seconds(0));
    CHECK(delta < std::chrono::seconds(5));

    // A reconnect re-arms it.
    driver->set_connected(false);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    driver->set_utc_date(far);
    CHECK(warns.load() == 2);
    driver->set_connected(false);
}

// The UTCDate readback is the client's value plus the time elapsed since the
// write, read from the driver's TaskClock (decision 0014), so the case moves
// it with advance() and never sleeps.
TEST_CASE(
    "OnStep Telescope Driver - a written UTCDate reads back advanced by the task clock and is dropped on reconnect",
    "[onstep][telescope][unit][utc-offset]") {
    alpacacore::test::FakeTaskClock clock;  // outlives the driver
    alpacacore::test::FakeMountServer server([](const std::string& chunk) {
        if (chunk.rfind(":SL", 0) == 0 || chunk.rfind(":SC", 0) == 0 || chunk.rfind(":SG", 0) == 0) {
            return std::string("1");
        }
        // A real mount clock (2026-01-15 12:00:00, offset 0), months from the
        // write: the connect's clock read rejects a reply that does not parse (#1626).
        if (chunk == ":GL#") {
            return std::string("12:00:00#");
        }
        if (chunk == ":GC#") {
            return std::string("01/15/26#");
        }
        if (chunk == ":GG#") {
            return std::string("+00:00#");
        }
        return std::string("0#");
    });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 50;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    // An hour from the host, so the re-seed after a reconnect cannot equal it.
    const auto written = std::chrono::system_clock::now() + std::chrono::hours(1);
    driver->set_utc_date(written);
    CHECK(driver->get_utc_date() == written);  // the fake clock has not moved

    clock.advance(std::chrono::seconds(60));
    (void)clock.wait_for_woken_settled(std::chrono::seconds(2));
    CHECK(driver->get_utc_date() == written + std::chrono::seconds(60));

    driver->set_connected(false);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    const auto reseeded = driver->get_utc_date();
    const auto gap = reseeded > written ? reseeded - written : written - reseeded;
    CHECK(gap > std::chrono::minutes(30));
    driver->set_connected(false);
}

// #1478: the host clock never stands in for the mount's. The connect reads the
// mount clock once; when that fails UTCDate throws InvalidOperation and reads
// nothing until a UTCDate write or the next connect, and an LST computation in
// between does not store the host clock either.
TEST_CASE("OnStep Telescope Driver - UTCDate throws InvalidOperation when the mount clock cannot be read",
          "[onstep][telescope][unit][utc-offset]") {
    alpacacore::test::FakeTaskClock clock;  // outlives the driver
    std::atomic<bool> clock_answers{false};
    std::atomic<bool> clock_silent{false};  // :GL# gets no reply: the read times out (#1626)
    std::atomic<int> clock_reads{0};
    // 0 valid, 1 garbled, 2 out of range, 3 not a calendar date (#1626), 4 a month past 255 (wraps in
    // std::chrono::month), 5 a three-digit year (2100-01-01 is a calendar date)
    std::atomic<int> date_reply{0};
    std::atomic<bool> hour_out_of_range{false};  // :GL# "25:00:00#"
    std::atomic<int> offset_reply{0};  // 0 "+00", 1 "+05:00" (EST), 2 garbled (#1626)
    alpacacore::test::FakeMountServer server([&](const std::string& chunk) {
        if (chunk.rfind(":GL", 0) == 0) {
            ++clock_reads;
            if (clock_silent) {
                return std::string();
            }
            if (hour_out_of_range) {
                return std::string("25:00:00#");
            }
            return clock_answers ? std::string("10:20:30#") : std::string("x#");
        }
        if (chunk.rfind(":GC", 0) == 0) {
            switch (date_reply.load()) {
                case 0:
                    return std::string("10/23/26#");
                case 2:
                    return std::string("13/40/26#");
                case 3:
                    return std::string("02/31/26#");
                case 4:
                    return std::string("257/01/26#");
                case 5:
                    return std::string("01/01/100#");
                default:
                    return std::string("xx#");
            }
        }
        if (chunk.rfind(":GG", 0) == 0) {
            return offset_reply == 0   ? std::string("+00#")
                   : offset_reply == 1 ? std::string("+05:00#")
                                       : std::string("?#");
        }
        if (chunk.rfind(":Gt", 0) == 0) {
            return std::string("+40*30#");
        }
        if (chunk.rfind(":Gg", 0) == 0) {
            return std::string("+085*30#");
        }
        if (chunk.rfind(":S", 0) == 0) {
            return std::string("1");  // bare-digit set acks (:SL/:SC/:SG)
        }
        return std::string("0#");
    });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 50;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn, clock);
    using namespace std::chrono;
    const auto mount_utc = sys_days{year{2026} / October / 23} + hours{10} + minutes{20} + seconds{30};

    SECTION("a clock the connect could not read: UTCDate throws, and no read is tried again") {
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        CHECK(clock_reads == 1);  // the connect's read
        require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
        REQUIRE_NOTHROW((void)driver->get_sidereal_time());  // computes LST from the host clock
        clock_answers = true;
        clock.advance(std::chrono::seconds(60));
        (void)clock.wait_for_woken_settled(std::chrono::seconds(2));
        require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
        (void)driver->get_device_state();
        (void)driver->get_device_state();
        CHECK(clock_reads == 1);  // UTCDate and DeviceState never read the clock (#1478)

        const auto written = sys_days{year{2026} / October / 24} + hours{1};
        REQUIRE_NOTHROW(driver->set_utc_date(written));
        CHECK(driver->get_utc_date() >= written);  // a UTCDate write sets it

        driver->set_connected(false);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        CHECK(clock_reads == 2);  // the next connect reads it again
        CHECK(driver->get_utc_date() == mount_utc);
    }

    SECTION("a clock read that times out at connect: the connect succeeds and UTCDate says why") {
        clock_silent = true;
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        CHECK(clock_reads == 1);
        try {
            (void)driver->get_utc_date();
            FAIL("UTCDate answered with no mount clock");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
            CHECK(std::string(ex.what()).find("Timeout") != std::string::npos);  // the reason is kept
        }
        CHECK(driver->get_connected());
        CHECK(driver->get_link_fault().empty());  // one slow clock read is not a silent mount
        REQUIRE_NOTHROW((void)driver->get_right_ascension());
        driver->set_connected(false);
    }

    SECTION("a clock the connect read: DeviceState makes no clock reads") {
        clock_answers = true;
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        CHECK(clock_reads == 1);
        (void)driver->get_device_state();
        (void)driver->get_device_state();
        CHECK(driver->get_utc_date() == mount_utc);
        CHECK(clock_reads == 1);
    }

    // #1626: a garbled date or offset reply used to leave the defaults
    // (2000-01-01, offset 0) to be stored as the mount clock.
    SECTION("a garbled :GC# or :GG# reply is an unreadable clock, not a wrong one") {
        clock_answers = true;
        for (const int reply : {1, 2, 3, 4, 5}) {  // garbled, out of range, 31 February, month 257, year 100
            INFO("date reply " << reply);
            date_reply = reply;
            REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
            require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
            driver->set_connected(false);
        }
        date_reply = 0;
        offset_reply = 2;
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
        driver->set_connected(false);
        offset_reply = 0;
        hour_out_of_range = true;  // a local time of 25:00:00
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
    }

    // #1626: OnStepX :GG# is the hours to ADD to local time to get UTC
    // (Site.command.cpp), so EST reads "+05:00" and UTC = local + 5 h. The
    // driver used to subtract it.
    SECTION("a non-zero :GG# offset is added to the local time") {
        clock_answers = true;
        offset_reply = 1;
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        CHECK(driver->get_utc_date() == mount_utc + hours{5});
    }

    driver->set_connected(false);
}

TEST_CASE("OnStep Telescope Driver - SiteElevation throws InvalidOperation until it is set",
          "[onstep][telescope][unit]") {
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = 1;
    auto unset = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);
    require_alpaca_error([&] { (void)unset->get_site_elevation(); }, alpacacore::AlpacaError::InvalidOperation);
    unset->set_site_elevation(250.0);
    CHECK(unset->get_site_elevation() == 250.0);

    auto configured = alpacacore::vendor::onstep::create_onstep_telescope_with_site(0, conn, std::nullopt, std::nullopt,
                                                                                    0.0, std::nullopt);
    CHECK(configured->get_site_elevation() == 0.0);  // a configured 0 m is a real place
}

TEST_CASE("OnStep Telescope Driver - DestinationSideOfPier and PulseGuide check arguments before the connection",
          "[onstep][telescope][unit]") {
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = 1;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto& [ra, dec] : std::vector<std::pair<double, double>>{
             {-0.1, 0.0}, {24.0, 0.0}, {nan, 0.0}, {12.0, -90.1}, {12.0, 90.1}, {12.0, nan}}) {
        require_alpaca_error([&] { (void)driver->get_destination_side_of_pier(ra, dec); },
                             alpacacore::AlpacaError::InvalidValue);
    }
    try {
        (void)driver->get_destination_side_of_pier(24.0, 0.0);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("DestinationSideOfPier: RightAscension must be in range") !=
              std::string::npos);
    }
    require_alpaca_error([&] { (void)driver->get_destination_side_of_pier(0.0, 90.0); },
                         alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&] { driver->pulse_guide(-1, 100); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->pulse_guide(4, 100); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->pulse_guide(0, -1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->pulse_guide(0, 10000); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->pulse_guide(3, 9999); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&] { driver->sync_to_coordinates(24.0, 0.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->sync_to_coordinates(12.0, 90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->sync_to_coordinates(12.0, 10.0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("OnStep Telescope Driver - a slow mount ack does not turn an accurate client clock into a warning",
          "[onstep][telescope][unit]") {
    // The offset is sampled BEFORE the mount write (review round 2 on #471):
    // set_time() is three serial round trips (:SL, :SC, :SG) against a 2 s
    // threshold, so a mount slow to ack them would otherwise make a
    // perfectly set client clock read as seconds out. With the offset
    // sampled after the write, this case logs a line; sampled before, nothing.
    //
    // The per-ack sleep sits between two fixed constants this test cannot
    // change: the summed round trip (3 acks) must clear the 2 s
    // kClientDisagreementWarn threshold, and each INDIVIDUAL ack must stay
    // under the wrapper's internal per-ack timeout (OnStepProtocolWrapper's
    // kSetAckTimeoutMs, 1000 ms -- private to the .cpp, so not reachable
    // from here, and NOT retried on expiry: it throws straight away). set_time()
    // always sends exactly three commands (:SL, :SC, :SG), and the driver
    // sends nothing else during set_utc_date(), so there is no fourth leg to
    // spread the load across -- a first attempt at this (950 ms/ack) widened
    // the threshold margin from 250 ms to 850 ms but crushed the per-ack
    // margin from 250 ms to 50 ms, trading one flake risk for a worse one
    // (review round on the #512/#513/#514 bundle).
    //
    // With 3 fixed legs, an equal per-leg sleep is what maximizes the sum for
    // a given per-leg ceiling, so 750 ms is the unique point that keeps both
    // margins at their largest simultaneously achievable value:
    //   threshold margin: 3 * 750 ms - 2000 ms = 250 ms
    //   per-ack margin:   1000 ms - 750 ms     = 250 ms
    // Any higher sleep buys threshold margin only by spending per-ack margin
    // (and vice versa) -- this is the balanced optimum, not an arbitrary
    // choice. The REQUIRE below pins that the round trip actually cleared the
    // threshold (so a regression in the threshold, the per-ack timeout, or
    // the ack count fails loudly here instead of silently vanishing), and the
    // set_utc_date() call itself is wrapped in REQUIRE_NOTHROW so that on a
    // loaded CI runner nudging one ack over its 1 s cap, the failure is a
    // clean, named assertion rather than an unattributed exception.
    constexpr auto kThreshold = alpacacore::util::HostClock::kClientDisagreementWarn;
    const auto per_ack_sleep = std::chrono::milliseconds(750);
    struct ProbeGuard {
        ProbeGuard() {
            alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return true; });
        }
        ~ProbeGuard() { alpacacore::util::ClientUtcWarning::set_host_synchronized_probe(nullptr); }
    } probe_guard;
    std::atomic<int> warns{0};
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Warn && component == "OnStep" &&
                message.find("Client UTCDate disagrees") != std::string::npos) {
                ++warns;
            }
        });

    alpacacore::test::FakeMountServer server([&](const std::string& chunk) {
        if (chunk.rfind(":SL", 0) == 0 || chunk.rfind(":SC", 0) == 0 || chunk.rfind(":SG", 0) == 0) {
            std::this_thread::sleep_for(per_ack_sleep);  // the slow ack, x3
            return std::string("1");
        }
        if (chunk.size() >= 2 && chunk[0] == 'K') {
            return std::string(1, chunk[1]) + "#";
        }
        return std::string("0#");
    });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 50;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    const auto call_started = std::chrono::steady_clock::now();
    // accurate client, slow mount; REQUIRE_NOTHROW so a per-ack timeout on a
    // loaded runner reports as a named assertion, not a bare uncaught exception.
    REQUIRE_NOTHROW(driver->set_utc_date(std::chrono::system_clock::now()));
    const auto elapsed = std::chrono::steady_clock::now() - call_started;
    // The case only proves the sampling order matters if the round trip it
    // drove actually cleared the threshold -- otherwise sampling AFTER the
    // write would read as agreeing too, and the CHECK below would pass
    // either way.
    REQUIRE(elapsed > kThreshold);
    CHECK(warns.load() == 0);
    driver->set_connected(false);
}

// #627: `x < min || x > max` is false for NaN, so NaN passed every range check
// and was stored (targets, elevation) or reached the mount (the site latitude
// and longitude setters check the connection only AFTER the range, so a NaN
// used to surface as NotConnected instead of InvalidValue).
TEST_CASE("OnStep Telescope Driver - non-finite input is rejected", "[onstep][telescope][unit][nonfinite]") {
    auto driver = make_driver(0);

    const double nan = std::numeric_limits<double>::quiet_NaN();

    SECTION("TargetDeclination") {
        require_alpaca_error([&]() { driver->set_target_declination(nan); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);
    }
    SECTION("TargetRightAscension") {
        require_alpaca_error([&]() { driver->set_target_right_ascension(nan); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); },
                             alpacacore::AlpacaError::ValueNotSet);
    }
    SECTION("SiteElevation") {
        require_alpaca_error([&]() { driver->set_site_elevation(nan); }, alpacacore::AlpacaError::InvalidValue);
    }
    SECTION("SiteLatitude") {
        require_alpaca_error([&]() { driver->set_site_latitude(nan); }, alpacacore::AlpacaError::InvalidValue);
    }
    SECTION("SiteLongitude") {
        require_alpaca_error([&]() { driver->set_site_longitude(nan); }, alpacacore::AlpacaError::InvalidValue);
    }
}

// #627: `validate_ra_dec` is `ra < 0 || ra >= 24` / `dec < -90 || dec > 90`,
// both false for NaN, so a NaN coordinate went through slew and sync to the
// mount. Both check the connection before validating, so this needs a
// connected driver.
TEST_CASE("OnStep Telescope Driver - non-finite slew and sync coordinates are rejected",
          "[onstep][telescope][unit][nonfinite]") {
    alpacacore::test::FakeMountServer server([](const std::string& chunk) {
        if (chunk.rfind(":SL", 0) == 0 || chunk.rfind(":SC", 0) == 0 || chunk.rfind(":SG", 0) == 0) {
            return std::string("1");
        }
        if (chunk.size() >= 2 && chunk[0] == 'K') {
            return std::string(1, chunk[1]) + "#";
        }
        return std::string("0#");
    });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 50;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    const double nan = std::numeric_limits<double>::quiet_NaN();

    SECTION("SlewToCoordinatesAsync RightAscension") {
        require_alpaca_error([&]() { driver->slew_to_coordinates_async(nan, 45.0); },
                             alpacacore::AlpacaError::InvalidValue);
    }
    SECTION("SlewToCoordinatesAsync Declination") {
        require_alpaca_error([&]() { driver->slew_to_coordinates_async(12.0, nan); },
                             alpacacore::AlpacaError::InvalidValue);
    }
    SECTION("SyncToCoordinates RightAscension") {
        require_alpaca_error([&]() { driver->sync_to_coordinates(nan, 45.0); }, alpacacore::AlpacaError::InvalidValue);
    }
    SECTION("SyncToCoordinates Declination") {
        require_alpaca_error([&]() { driver->sync_to_coordinates(12.0, nan); }, alpacacore::AlpacaError::InvalidValue);
    }
    // The alt/az entry points convert to RA/Dec through their own range check
    // (compute_alt_az_target_locked), which NaN also passed.
    SECTION("SlewToAltAzAsync Altitude") {
        require_alpaca_error([&]() { driver->slew_to_alt_az_async(nan, 90.0); }, alpacacore::AlpacaError::InvalidValue);
    }
    SECTION("SlewToAltAzAsync Azimuth") {
        require_alpaca_error([&]() { driver->slew_to_alt_az_async(45.0, nan); }, alpacacore::AlpacaError::InvalidValue);
    }

    driver->set_connected(false);
}

// #742: MoveAxis(axis, 0) swallowed both direction stops and cleared the
// manual-slew flag, so a stop lost on a dead link read as Slewing false while
// the mount kept moving. The stops are blind sends, which a responder cannot
// fail, so the fake resets the connection instead: every stop after that fails.
TEST_CASE("OnStep Telescope Driver - MoveAxis at rate 0 reports a stop it could not send",
          "[onstep][telescope][unit]") {
    alpacacore::test::FakeMountServer server([](const std::string& chunk) {
        if (chunk.rfind(":SL", 0) == 0 || chunk.rfind(":SC", 0) == 0 || chunk.rfind(":SG", 0) == 0) {
            return std::string("1");
        }
        // Status: not tracking, not slewing, not parked. The canned "0#"
        // lacks the 'N' and so reads as a GOTO in progress, which would make
        // Slewing true whatever MoveAxis did.
        if (chunk.rfind(":GU", 0) == 0) {
            return std::string("nNp#");
        }
        return std::string("0#");
    });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 50;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    REQUIRE_FALSE(driver->get_slewing());  // only the axis move below makes it true
    driver->move_axis(0, 1.0);
    REQUIRE(driver->get_slewing());

    REQUIRE(server.drop_connections());
    try {
        driver->move_axis(0, 0.0);
        FAIL("MoveAxis(0, 0) returned success although the stop could not be sent");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).rfind("MoveAxis stop failed: ", 0) == 0);
    }
    CHECK(driver->get_slewing());  // the mount may still be moving

    driver->set_connected(false);
}

namespace {

// #1378: a fake OnStepX for the MoveAxis re-send. Counts every :Me# sent and
// answers :GU# with or without 'g' (a guide-style move is running).
struct MoveAxisFake {
    std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::string> sent = std::make_shared<std::string>();
    std::shared_ptr<std::atomic<bool>> guide_active = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> pulse_guide = std::make_shared<std::atomic<bool>>(false);
    // :GA# in tenths of a degree, and the :Gh#/:Go# limits in degrees.
    std::shared_ptr<std::atomic<int>> altitude_tenths = std::make_shared<std::atomic<int>>(450);
    std::shared_ptr<std::atomic<int>> horizon = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> overhead = std::make_shared<std::atomic<int>>(90);
    // Every driver timer runs on this (decision 0005): the re-send interval,
    // the 2 s status cache. Declared before `server` and the driver, so it
    // outlives both.
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeMountServer server;

    explicit MoveAxisFake(bool firmware_sends_g)
        : server([m = mutex, sent = sent, g = guide_active, pg = pulse_guide, alt = altitude_tenths, hz = horizon,
                  oh = overhead, firmware_sends_g](const std::string& chunk) {
              {
                  std::lock_guard<std::mutex> lock(*m);
                  *sent += chunk;
              }
              if (chunk.rfind(":SL", 0) == 0 || chunk.rfind(":SC", 0) == 0 || chunk.rfind(":SG", 0) == 0 ||
                  chunk.rfind(":ST", 0) == 0) {
                  return std::string("1");  // bare-digit acks (:ST too, #1379)
              }
              if (chunk.rfind(":GA", 0) == 0) {
                  const int tenths = alt->load();
                  const int whole = std::abs(tenths) / 10;
                  const int minutes = (std::abs(tenths) % 10) * 6;
                  char reply[32];
                  std::snprintf(reply, sizeof(reply), "%c%02d*%02d:00#", tenths < 0 ? '-' : '+', whole, minutes);
                  return std::string(reply);
              }
              if (chunk.rfind(":GZ", 0) == 0) {
                  return std::string("180*00:00#");
              }
              if (chunk.rfind(":Gh", 0) == 0) {
                  char reply[16];
                  std::snprintf(reply, sizeof(reply), "%+d*#", hz->load());
                  return std::string(reply);
              }
              if (chunk.rfind(":Go", 0) == 0) {
                  char reply[16];
                  std::snprintf(reply, sizeof(reply), "%02d*#", oh->load());
                  return std::string(reply);
              }
              if (chunk.rfind(":GU", 0) == 0) {
                  if (pg->load()) {
                      return std::string("nNpG#");  // OnStepX drops 'g' while a pulse guide runs
                  }
                  return std::string(firmware_sends_g && g->load() ? "nNpg#" : "nNp#");
              }
              // Blind move and stop commands draw no reply.
              if (chunk.rfind(":M", 0) == 0 || chunk.rfind(":Q", 0) == 0 || chunk.rfind(":R", 0) == 0) {
                  return std::string();
              }
              return std::string("0#");
          }) {}

    std::size_t count(const std::string& needle) const {
        std::lock_guard<std::mutex> lock(*mutex);
        std::size_t n = 0;
        for (auto pos = sent->find(needle); pos != std::string::npos; pos = sent->find(needle, pos + 1)) {
            ++n;
        }
        return n;
    }

    std::unique_ptr<alpacacore::TelescopeDriver> connect() {
        alpacacore::vendor::onstep::ConnectionInfo conn;
        conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
        conn.host = "127.0.0.1";
        conn.tcp_port = server.port();
        conn.response_timeout_ms = 50;
        return alpacacore::vendor::onstep::create_onstep_telescope(0, conn, clock);
    }

    // Moves virtual time by `d` and waits until every task it woke has run
    // and is waiting again or has returned (the re-send body's tick).
    void advance(std::chrono::nanoseconds d) {
        clock.advance(d);
        REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    }

    // Starts or changes a move, and waits until the new re-send body waits on
    // the clock: a replaced body may still be registered for a moment, so the
    // rendezvous is the wait count, which only the new body's wait raises.
    void start_move(alpacacore::TelescopeDriver& driver, int axis, double rate) {
        const auto before = clock.wait_count();
        driver.move_axis(axis, rate);
        REQUIRE(clock.wait_for_wait_count(before + 1, std::chrono::seconds(5)));
    }

    // One re-send interval (7 s) with the re-send body waiting for it.
    void resend_tick() {
        REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
        advance(std::chrono::seconds(7));
    }

    // Serial I/O, not a driver timer: waits (real time) until the server has
    // logged `n` of `needle`; false at `timeout`.
    bool logged(const std::string& needle, std::size_t n,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (count(needle) < n) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
};

}  // namespace

// #1378 (review): OnStepX drops 'g' while a pulse guide runs ('G'), even
// with a MoveAxis still running, so 'G' must not end the move. Those ticks
// send nothing; the re-send resumes once 'g' is back.
TEST_CASE("OnStep Telescope Driver - a pulse guide does not end a running MoveAxis", "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    fake.guide_active->store(true);
    fake.start_move(*driver, 0, 1.0);
    fake.resend_tick();  // the re-send has seen 'g'
    REQUIRE(fake.logged(":Me#", 2));
    fake.pulse_guide->store(true);
    fake.resend_tick();
    CHECK_FALSE(fake.logged(":Me#", 3, std::chrono::milliseconds(300)));  // nothing sent during the pulse
    CHECK(driver->get_slewing());

    fake.pulse_guide->store(false);
    fake.resend_tick();
    CHECK(fake.logged(":Me#", 3));
    CHECK(driver->get_slewing());

    driver->move_axis(0, 0.0);
    driver->set_connected(false);
}

// #1378 (review): Tracking patches the status cache without a :GU# read, so
// the 'g' it keeps is from before the move and must not end the move.
TEST_CASE("OnStep Telescope Driver - a Tracking write right after MoveAxis does not end the move",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    // A first move teaches the driver that this firmware sends 'g': a :GU#
    // read past the 2 s cache, while the move runs.
    fake.guide_active->store(true);
    fake.start_move(*driver, 0, 1.0);
    fake.advance(std::chrono::milliseconds(2100));
    REQUIRE(driver->get_slewing());
    fake.guide_active->store(false);
    driver->move_axis(0, 0.0);
    fake.advance(std::chrono::milliseconds(2100));
    (void)driver->get_tracking();                   // a :GU# without 'g' is now in the cache
    fake.advance(std::chrono::milliseconds(2100));  // and stale

    fake.guide_active->store(true);
    fake.start_move(*driver, 0, 1.0);
    driver->set_tracking(true);  // marks the stale cache fresh without a :GU# read
    CHECK(driver->get_slewing());

    driver->move_axis(0, 0.0);
    driver->set_connected(false);
}

// #1378 (review): OnStepX stops an equatorial mount at the horizon and
// overhead limits only on the crossing, so a re-send must never go out within
// 1 degree of either limit, or past it. 90 overhead means no overhead limit.
TEST_CASE("OnStep Telescope Driver - the MoveAxis re-send stays clear of the altitude limits",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    fake.guide_active->store(true);
    fake.horizon->store(10);
    fake.overhead->store(80);
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    // True when one re-send tick sent the move again.
    const auto resends_at = [&](int altitude_tenths, int overhead) {
        fake.altitude_tenths->store(altitude_tenths);
        fake.overhead->store(overhead);
        const auto before = fake.count(":Mn#");
        fake.start_move(*driver, 1, 1.0);
        REQUIRE(fake.logged(":Mn#", before + 1));  // the start
        fake.resend_tick();
        const bool resent = fake.logged(":Mn#", before + 2, std::chrono::milliseconds(300));
        driver->move_axis(1, 0.0);
        return resent;
    };

    CHECK(resends_at(450, 80));        // well inside both limits
    CHECK_FALSE(resends_at(105, 80));  // 0.5 deg above the horizon limit
    CHECK_FALSE(resends_at(50, 80));   // below the horizon limit
    CHECK_FALSE(resends_at(795, 80));  // 0.5 deg under the overhead limit
    CHECK(resends_at(895, 90));        // overhead limit off
    CHECK_FALSE(resends_at(450, 0));   // an overhead reply out of range sends nothing

    driver->set_connected(false);
}

// #1378 (review): OnStepX times each axis on its own, so a MoveAxis on one
// axis must not push the other axis's re-send back: not when it starts later,
// and not when a joystick-style client updates it faster than the interval.
TEST_CASE("OnStep Telescope Driver - a MoveAxis on one axis does not delay the other axis's re-send",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    fake.guide_active->store(true);
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    fake.start_move(*driver, 0, 1.0);  // axis 0 due at 7 s
    fake.advance(std::chrono::seconds(4));
    fake.start_move(*driver, 1, 1.0);  // a new body; axis 0 is still due at 7 s, not 11 s
    fake.advance(std::chrono::seconds(3));
    CHECK(fake.logged(":Me#", 2));
    CHECK(fake.count(":Mn#") == 1);  // axis 1 is not due yet

    // Axis 1 updated every 3 s, faster than the interval: axis 0 keeps its
    // own 7 s rhythm (due again at 14 s).
    for (int i = 0; i < 3; ++i) {
        fake.advance(std::chrono::seconds(3));
        fake.start_move(*driver, 1, 1.0 + 0.1 * i);
    }
    CHECK(fake.logged(":Me#", 3));

    driver->move_axis(0, 0.0);
    driver->move_axis(1, 0.0);
    driver->set_connected(false);
}

// #1378: OnStepX ends a :Me# move after GUIDE_TIME_LIMIT (10 s by default),
// so a running MoveAxis must be re-sent until the client stops it, and never
// after.
TEST_CASE("OnStep Telescope Driver - a running MoveAxis is re-sent until it is stopped", "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    fake.guide_active->store(true);
    fake.start_move(*driver, 0, 1.0);
    fake.resend_tick();
    fake.resend_tick();
    CHECK(fake.logged(":Me#", 3));  // the start plus two re-sends

    driver->move_axis(0, 0.0);
    const auto after_stop = fake.count(":Me#");
    fake.advance(std::chrono::seconds(7));
    CHECK_FALSE(fake.logged(":Me#", after_stop + 1, std::chrono::milliseconds(300)));
    CHECK_FALSE(driver->get_slewing());

    driver->set_connected(false);
}

// #1378: once :GU# has shown 'g', its absence means OnStepX ended the move (a
// limit, or its time limit), so Slewing must read false and the re-send must
// stop.
TEST_CASE("OnStep Telescope Driver - Slewing drops when OnStepX ends a MoveAxis by itself",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    fake.guide_active->store(true);
    fake.start_move(*driver, 1, -1.0);
    fake.resend_tick();  // its :GU# showed 'g'
    REQUIRE(fake.logged(":Ms#", 2));
    REQUIRE(driver->get_slewing());

    fake.guide_active->store(false);
    fake.advance(std::chrono::milliseconds(2100));  // past the 2 s status cache
    CHECK_FALSE(driver->get_slewing());

    const auto after_end = fake.count(":Ms#");
    fake.advance(std::chrono::seconds(7));
    CHECK_FALSE(fake.logged(":Ms#", after_end + 1, std::chrono::milliseconds(300)));

    driver->set_connected(false);
}

// #1378 (review): a stop from a hand controller or another app, or a horizon
// limit, ends the move on the mount. The re-send must see that on its own,
// with no client polling Slewing, and must not start the move again, not even
// when the mount shows a move again later (the hand controller's own).
TEST_CASE("OnStep Telescope Driver - the MoveAxis re-send never restarts a move the mount ended",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    fake.guide_active->store(true);
    fake.start_move(*driver, 0, 1.0);
    fake.resend_tick();
    REQUIRE(fake.logged(":Me#", 2));  // re-sending while the mount shows the move

    fake.guide_active->store(false);  // stopped at the mount
    fake.resend_tick();               // the body sees it and ends
    const auto after_end = fake.count(":Me#");
    fake.guide_active->store(true);  // a move the hand controller started
    fake.advance(std::chrono::seconds(7));
    CHECK_FALSE(fake.logged(":Me#", after_end + 1, std::chrono::milliseconds(300)));
    fake.guide_active->store(false);
    CHECK_FALSE(driver->get_slewing());

    driver->set_connected(false);
}

// #1378: a move the mount does not show ('g') is never re-sent: firmware
// without the letter, or a move the firmware let lapse (its time limit, or
// the 5 s cap while its soft limits are off). Slewing stays true until the
// client stops the move, as before. #1461: the body ends after that first
// read instead of reading :GU# every 7 s for nothing.
TEST_CASE("OnStep Telescope Driver - a MoveAxis the mount does not show is never re-sent",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(false);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    const auto status_reads = fake.count(":GU#");
    fake.start_move(*driver, 0, -1.0);
    fake.resend_tick();
    REQUIRE(fake.logged(":GU#", status_reads + 1));
    fake.advance(std::chrono::seconds(21));                               // three more intervals
    CHECK(fake.count(":GU#") == status_reads + 1);                        // no read after the first
    CHECK_FALSE(fake.logged(":Mw#", 2, std::chrono::milliseconds(300)));  // the start only
    CHECK(driver->get_slewing());

    driver->move_axis(0, 0.0);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

namespace {

// Counts the OnStep WARN lines that contain `needle`; restores the sink when it goes.
class WarnCounter {
public:
    explicit WarnCounter(std::string needle) : needle_(std::move(needle)) {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn && component == "OnStep" &&
                    message.find(needle_) != std::string_view::npos) {
                    ++count_;
                }
            });
    }
    ~WarnCounter() { alpacacore::logging::set_log_sink(previous_); }
    WarnCounter(const WarnCounter&) = delete;
    WarnCounter& operator=(const WarnCounter&) = delete;

    int count() const { return count_.load(); }

private:
    alpacacore::logging::LogSink previous_ = alpacacore::logging::get_log_sink();
    std::string needle_;
    std::atomic<int> count_{0};
};

}  // namespace

// #1458: the re-send slot can refuse its body (no thread, or replaced bodies
// still finishing) after the move went out. The bump has then superseded the
// running body, so nothing would keep the move going while Slewing read true:
// MoveAxis stops the axis and fails.
TEST_CASE("OnStep Telescope Driver - a MoveAxis whose re-send cannot start stops the axis and fails",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    const auto refuse = [](const std::function<void()>&) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    };
    alpacacore::vendor::onstep::set_move_axis_resend_spawn_for_testing(*driver, refuse);
    fake.guide_active->store(true);
    const auto refused_move = [&](int axis) {
        try {
            driver->move_axis(axis, 1.0);
            FAIL("MoveAxis returned success although nothing keeps the move going");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).find("the axis was stopped") != std::string::npos);
        }
    };

    SECTION("axis 0") {
        refused_move(0);
        CHECK(fake.logged(":Me#", 1));
        CHECK(fake.logged(":Qe#", 1));
        CHECK(fake.logged(":Qw#", 1));
    }
    SECTION("axis 1") {
        refused_move(1);
        CHECK(fake.logged(":Mn#", 1));
        CHECK(fake.logged(":Qn#", 1));
        CHECK(fake.logged(":Qs#", 1));
    }
    fake.guide_active->store(false);  // the mount stopped
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// #1458: when the stop after a refused re-send fails too, the message says
// so and Slewing stays true (#742): the mount may still be moving.
TEST_CASE("OnStep Telescope Driver - a refused re-send whose stop fails too leaves Slewing true",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    // The link drops as the slot refuses: the stops that follow cannot be sent.
    alpacacore::vendor::onstep::set_move_axis_resend_spawn_for_testing(
        *driver, [&fake](const std::function<void()>&) -> std::thread {
            (void)fake.server.drop_connections();
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        });

    fake.guide_active->store(true);
    try {
        driver->move_axis(0, 1.0);
        FAIL("MoveAxis returned success although nothing keeps the move going");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("stopping the axis failed too") != std::string::npos);
    }
    CHECK(driver->get_slewing());
    driver->set_connected(false);
}

// #1458: an AbortSlew that ended the move while the refused start was still
// in the slot owns the motion: the refused call reports the refusal as it
// came and sends no stops of its own, which could end a newer GOTO or Park.
TEST_CASE("OnStep Telescope Driver - a refused re-send stops nothing after an AbortSlew", "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    // The slot calls the spawn hook under its own lock, which AbortSlew's
    // cancel() needs: the AbortSlew runs on its own thread, and the hook
    // fails the start once its stops went out. It still holds the driver
    // lock then, so it finishes before MoveAxis looks at the axis again.
    std::thread abort;
    alpacacore::vendor::onstep::set_move_axis_resend_spawn_for_testing(
        *driver, [&](const std::function<void()>&) -> std::thread {
            abort = std::thread([&driver] { driver->abort_slew(); });
            (void)(fake.logged(":Qe#", 1) && fake.logged(":Qw#", 1) && fake.logged(":Qn#", 1) &&
                   fake.logged(":Qs#", 1));
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        });

    fake.guide_active->store(true);
    try {
        driver->move_axis(0, 1.0);
        FAIL("MoveAxis returned success although its re-send did not start");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("MoveAxis could not keep") == std::string::npos);
    }
    REQUIRE(abort.joinable());  // the spawn hook ran: a regression fails here, not in join()
    abort.join();
    REQUIRE(fake.logged(":Qe#", 1));
    CHECK_FALSE(fake.logged(":Qe#", 2, std::chrono::milliseconds(200)));  // the AbortSlew's stop only
    driver->set_connected(false);
}

// #1458: a newer MoveAxis that went out while the refused start was still
// in the slot owns the motion: the refused call reports the refusal as it
// came and stops nothing.
TEST_CASE("OnStep Telescope Driver - a refused re-send stops nothing after a newer MoveAxis",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    // The first start waits until the newer MoveAxis went out, then fails;
    // the starts after it get a real thread.
    std::mutex latch_mutex;
    std::condition_variable latch_cv;
    bool released = false;
    std::atomic<int> starts{0};
    alpacacore::vendor::onstep::set_move_axis_resend_spawn_for_testing(
        *driver, [&](std::function<void()> body) -> std::thread {
            if (starts.fetch_add(1) == 0) {
                std::unique_lock<std::mutex> lock(latch_mutex);
                latch_cv.wait(lock, [&] { return released; });
                throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
            }
            return std::thread(std::move(body));
        });

    fake.guide_active->store(true);
    std::exception_ptr first_error;
    std::exception_ptr newer_error;
    std::thread first([&] {
        try {
            driver->move_axis(0, 1.0);
        } catch (...) {
            first_error = std::current_exception();  // Catch2 asserts only on the test thread
        }
    });
    std::thread newer([&] {
        while (starts.load() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        try {
            driver->move_axis(1, 1.0);
        } catch (...) {
            newer_error = std::current_exception();
        }
    });
    CHECK(fake.logged(":Mn#", 1));  // the newer move went out (and bumped) while the first start waits
    {
        std::lock_guard<std::mutex> lock(latch_mutex);
        released = true;
    }
    latch_cv.notify_all();
    first.join();
    newer.join();
    CHECK_FALSE(newer_error);
    REQUIRE(first_error);
    try {
        std::rethrow_exception(first_error);
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("could not start a background thread") != std::string::npos);
        CHECK(std::string(ex.what()).find("could not keep the move going") == std::string::npos);
    }
    CHECK(fake.count(":Qe#") == 0);
    CHECK(fake.count(":Qw#") == 0);
    fake.resend_tick();
    CHECK(fake.logged(":Mn#", 2));  // the newer move keeps its re-send
    driver->move_axis(1, 0.0);
    driver->set_connected(false);
}

// #1461: a pulse guide ('G') hides 'g', so a first tick that sees 'G' and no
// 'g' does not end the body: the move is re-sent once the pulse is over.
TEST_CASE("OnStep Telescope Driver - a pulse guide at the first re-send tick does not end the re-send",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    fake.guide_active->store(true);
    fake.pulse_guide->store(true);  // 'G' and no 'g' from the start
    fake.start_move(*driver, 0, 1.0);
    fake.resend_tick();
    fake.pulse_guide->store(false);
    fake.resend_tick();  // the body still waits for it
    CHECK(fake.logged(":Me#", 2));

    driver->move_axis(0, 0.0);
    driver->set_connected(false);
}

// #1459: a MoveAxis(axis, 0) whose stop cannot be sent still ends that axis's
// re-send (the flag drops before the stop), so no tick runs for it later. A
// dropped TCP link fails the write while the wrapper still reads the link as
// alive, as a serial write error that is not a loss would.
TEST_CASE("OnStep Telescope Driver - a MoveAxis stop that cannot be sent still ends the re-send",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    WarnCounter resend_warns("MoveAxis re-send");

    fake.guide_active->store(true);
    fake.start_move(*driver, 0, 1.0);
    fake.resend_tick();
    REQUIRE(fake.logged(":Me#", 2));
    REQUIRE(fake.server.drop_connections());
    CHECK_THROWS_AS(driver->move_axis(0, 0.0), alpacacore::AlpacaException);
    fake.advance(std::chrono::seconds(14));  // two intervals: no tick may run
    CHECK(resend_warns.count() == 0);
    driver->set_connected(false);
}

// #1459: a MoveAxis whose command cannot be sent drops its own axis's
// earlier move from the re-send first, so the old direction is not kept
// going for a move the client changed.
TEST_CASE("OnStep Telescope Driver - a MoveAxis that cannot be sent ends its axis's earlier re-send",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    WarnCounter resend_warns("MoveAxis re-send");

    fake.guide_active->store(true);
    fake.start_move(*driver, 0, 1.0);
    fake.resend_tick();
    REQUIRE(fake.logged(":Me#", 2));
    REQUIRE(fake.server.drop_connections());
    CHECK_THROWS_AS(driver->move_axis(0, -1.0), alpacacore::AlpacaException);
    fake.advance(std::chrono::seconds(14));  // two intervals: no tick may run
    CHECK(resend_warns.count() == 0);
    driver->set_connected(false);
}

// #1459: a MoveAxis whose command cannot be sent leaves the other axis's
// re-send running: the generation moves only once the move went out. The
// kept body's next tick shows it: its status read fails on the dropped link.
TEST_CASE("OnStep Telescope Driver - a MoveAxis that cannot be sent leaves the other axis's re-send running",
          "[onstep][telescope][unit]") {
    MoveAxisFake fake(true);
    REQUIRE(fake.server.ok());
    auto driver = fake.connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    WarnCounter skipped("MoveAxis re-send skipped");

    fake.guide_active->store(true);
    fake.start_move(*driver, 0, 1.0);
    fake.resend_tick();
    REQUIRE(fake.logged(":Me#", 2));
    REQUIRE(fake.server.drop_connections());
    CHECK_THROWS_AS(driver->move_axis(1, 1.0), alpacacore::AlpacaException);
    fake.resend_tick();  // axis 0 is due again
    CHECK(skipped.count() >= 1);
    driver->set_connected(false);
}

// #781: AbortSlew swallowed every direction stop and then cleared Slewing, so a
// stop lost on a dead link read as a clean abort while the mount kept moving.
// A failed stop must surface as DriverException after every stop was tried, and
// must leave Slewing alone; the success path keeps clearing it.
TEST_CASE("OnStep Telescope Driver - AbortSlew reports a stop it could not send", "[onstep][telescope][unit]") {
    auto log = std::make_shared<std::vector<std::string>>();
    auto log_mutex = std::make_shared<std::mutex>();
    alpacacore::test::FakeMountServer server([log, log_mutex](const std::string& chunk) {
        {
            std::lock_guard<std::mutex> lock(*log_mutex);
            log->push_back(chunk);
        }
        if (chunk.rfind(":SL", 0) == 0 || chunk.rfind(":SC", 0) == 0 || chunk.rfind(":SG", 0) == 0) {
            return std::string("1");
        }
        if (chunk.rfind(":GU", 0) == 0) {
            return std::string("nNp#");
        }
        return std::string("0#");
    });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 50;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    driver->move_axis(0, 1.0);
    REQUIRE(driver->get_slewing());

    SECTION("success clears Slewing and sends the abort and all four stops") {
        {
            std::lock_guard<std::mutex> lock(*log_mutex);
            log->clear();
        }
        REQUIRE_NOTHROW(driver->abort_slew());
        CHECK_FALSE(driver->get_slewing());
        std::lock_guard<std::mutex> lock(*log_mutex);
        const auto count = [&](const char* needle) {
            return std::count_if(log->begin(), log->end(),
                                 [&](const std::string& c) { return c.find(needle) != std::string::npos; });
        };
        CHECK(count(":Q#") == 1);
        CHECK(count(":Qe#") >= 1);
        CHECK(count(":Qw#") >= 1);
        CHECK(count(":Qn#") >= 1);
        CHECK(count(":Qs#") >= 1);
    }

    SECTION("a dropped link throws DriverException and leaves Slewing true") {
        REQUIRE(server.drop_connections());
        try {
            driver->abort_slew();
            FAIL("AbortSlew returned success although the stops could not be sent");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).rfind("AbortSlew stop failed: ", 0) == 0);
        }
        CHECK(driver->get_slewing());
    }

    driver->set_connected(false);
}

TEST_CASE("OnStep protocol wrapper - :GU# pier side is T/W/o and not the mount-type E", "[onstep][telescope][unit]") {
    auto status_mutex = std::make_shared<std::mutex>();
    auto reply = std::make_shared<std::string>();
    alpacacore::test::FakeMountServer server([reply, status_mutex](const std::string& chunk) {
        if (chunk.rfind(":GU", 0) == 0) {
            std::lock_guard<std::mutex> lock(*status_mutex);
            return *reply;
        }
        return std::string("0#");
    });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 200;
    auto& wrapper = alpacacore::vendor::onstep::OnStepProtocolWrapper::instance();
    REQUIRE(wrapper.connect(conn));
    // The wrapper is process-wide: disconnect on every exit, so a throw here
    // cannot leave it connected and fail every later OnStep test.
    struct DisconnectOnExit {
        alpacacore::vendor::onstep::OnStepProtocolWrapper& w;
        ~DisconnectOnExit() { w.disconnect(); }
    } disconnect_on_exit{wrapper};

    const auto status_for = [&](const std::string& raw) {
        {
            std::lock_guard<std::mutex> lock(*status_mutex);
            *reply = raw + "#";
        }
        return wrapper.get_status();
    };

    // Real TeSeek HM-17 (OnStepX 10.23a) replies.
    auto west = status_for("nNpEW180");
    CHECK(west.side_of_pier == 1);
    CHECK_FALSE(west.is_tracking);
    CHECK_FALSE(west.is_slewing);

    auto east = status_for("nNpET1:0");
    CHECK(east.side_of_pier == 0);

    auto parked = status_for("nNPHEo180");
    CHECK(parked.side_of_pier == -1);
    CHECK(parked.is_parked);
    CHECK(parked.is_at_home);

    auto tracking_east = status_for("NpET180");
    CHECK(tracking_east.side_of_pier == 0);
    CHECK(tracking_east.is_tracking);
}

TEST_CASE("OnStep protocol wrapper - a long reply costs no time per character", "[onstep][telescope][unit]") {
    // The fake answers at once, so the wire adds nothing: what is measured is
    // the read loop. A pause after every character costs 10 ms each (#1242).
    const std::string long_reply(60, 'x');
    alpacacore::test::FakeMountServer server([long_reply](const std::string&) { return long_reply + "#"; });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 2000;
    auto& wrapper = alpacacore::vendor::onstep::OnStepProtocolWrapper::instance();
    REQUIRE(wrapper.connect(conn));
    struct DisconnectOnExit {
        alpacacore::vendor::onstep::OnStepProtocolWrapper& w;
        ~DisconnectOnExit() { w.disconnect(); }
    } disconnect_on_exit{wrapper};

    const auto started = std::chrono::steady_clock::now();
    const std::string reply = wrapper.send_command(":GVP#");
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(reply == long_reply);
    // 60 characters at 10 ms each would take 600 ms; the bound leaves room
    // for a slow sanitizer build.
    CHECK(elapsed < std::chrono::milliseconds(200));
}

namespace {

// A fake OnStepX whose :GU# reply the test scripts, recording every command.
struct ScriptedOnStep {
    std::mutex mutex;
    std::string status = "nNpEW180#";
    std::string ms_reply = "0";     // slew accepted
    std::string st_reply = "1";     // :ST tracking accepted (bare digit)
    std::string hq_reply = "1";     // :hQ# set park accepted
    std::string hr_reply = "1";     // :hR# unpark accepted
    std::string cm_reply = "N/A#";  // :CM# synced
    std::string ge_reply = "00#";   // :GE# last command error
    std::vector<std::string> log;

    void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lock(mutex);
        status = s;
    }
    void set_ms_reply(const std::string& r) {
        std::lock_guard<std::mutex> lock(mutex);
        ms_reply = r;
    }
    // A numeric command answered '0', and the :GE# code that says why.
    void refuse(const char* command, const std::string& ge) {
        std::lock_guard<std::mutex> lock(mutex);
        const std::string c = command;
        (c == ":ST" ? st_reply : c == ":hQ" ? hq_reply : hr_reply) = "0";
        ge_reply = ge;
    }
    void set_cm_reply(const std::string& r) {
        std::lock_guard<std::mutex> lock(mutex);
        cm_reply = r;
    }
    std::size_t count(const char* needle) {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(std::count_if(
            log.begin(), log.end(), [&](const std::string& c) { return c.find(needle) != std::string::npos; }));
    }
    void clear_log() {
        std::lock_guard<std::mutex> lock(mutex);
        log.clear();
    }
    std::string reply(const std::string& chunk) {
        std::lock_guard<std::mutex> lock(mutex);
        log.push_back(chunk);
        if (chunk.rfind(":GU", 0) == 0) return status;
        if (chunk.rfind(":ST", 0) == 0) return st_reply;
        if (chunk.rfind(":S", 0) == 0) return "1";        // bare-digit set acks (:Sr/:Sd/:SL/:SC/:SG)
        if (chunk.rfind(":MS", 0) == 0) return ms_reply;  // bare digit, no '#'
        if (chunk.rfind(":hP", 0) == 0) return "1";  // park accepted (bare digit, #1430)
        if (chunk.rfind(":hQ", 0) == 0) return hq_reply;
        if (chunk.rfind(":hR", 0) == 0) return hr_reply;
        if (chunk.rfind(":CM", 0) == 0) return cm_reply;
        if (chunk.rfind(":GE", 0) == 0) return ge_reply;
        if (chunk.rfind(":GRa", 0) == 0) return "12:00:00.0000#";
        if (chunk.rfind(":GDe", 0) == 0) return "+45*00:00.000#";
        return "0#";
    }
};

// The driver runs on `clock` (decision 0005): moves virtual time past the
// 2 s status cache and the slew grace, so the next Slewing read fetches the
// scripted in-progress status with a :GU#, and the driver has seen the slew
// running and will notice its end. Call right after set_status().
void observe_slewing(alpacacore::TelescopeDriver& driver, ScriptedOnStep& mount,
                     alpacacore::test::FakeTaskClock& clock) {
    const std::size_t polls_before = mount.count(":GU#");
    clock.advance(std::chrono::milliseconds(2100));
    REQUIRE(driver.get_slewing());
    REQUIRE(mount.count(":GU#") > polls_before);
}

// Moves virtual time past the status cache; true when Slewing then reads false.
bool wait_slew_done(alpacacore::TelescopeDriver& driver, alpacacore::test::FakeTaskClock& clock) {
    clock.advance(std::chrono::milliseconds(2100));
    return !driver.get_slewing();
}

// Runs a synchronous slew on a thread and steps `clock` in the driver's 250 ms
// poll steps until it returns. `while_polling` runs once, after the slew's first
// poll wait has begun. The slew's exception, if any, is rethrown here.
void run_synchronous_slew(alpacacore::test::FakeTaskClock& clock, const std::function<void()>& slew,
                          const std::function<void()>& while_polling = {}) {
    std::exception_ptr error;
    std::atomic<bool> done{false};
    std::thread runner([&] {
        try {
            slew();
        } catch (...) {
            error = std::current_exception();  // Catch2 asserts only on the test thread
        }
        done = true;
    });
    bool first = true;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done && std::chrono::steady_clock::now() < give_up) {
        if (!clock.wait_for_waiters(1, std::chrono::milliseconds(50))) {
            continue;  // the slew is talking to the mount, or has returned
        }
        if (first && while_polling) {
            while_polling();
        }
        first = false;
        clock.advance(std::chrono::milliseconds(250));
        (void)clock.wait_for_woken_settled(std::chrono::seconds(5));
    }
    runner.join();
    CHECK(done);
    if (error) {
        std::rethrow_exception(error);
    }
}

// #1430: a scripted OnStepX 10.23a for Park and FindHome. A stop (:Q...) ends
// the guide move, GOTO, home or pulse guide after `brake_ms` of braking ('g',
// no 'N', 'h' or 'G' stays until then); :hP# and :hC# are refused with
// CE_SLEW_IN_MOTION (22) while any of them shows (Park::request,
// Home::request), or while
// `refuse_in_motion` refusals are left, or with `refuse_code`; :GE# answers
// the code of the command before it, which any other command resets, as
// OnStepX does (ProcessCmds.cpp).
struct ParkHomeFake {
    std::mutex mutex;
    std::vector<std::string> log;
    std::string version = "10.23a";
    bool guide_active = false;
    bool goto_active = false;
    bool parking = false;
    bool homing = false;
    bool pulse_active = false;  // 'G'; a pulse guide ends only on a stop here
    bool sends_g = true;        // false: firmware without 'g' in :GU#
    bool tracking = true;       // false: 'n' in :GU#
    bool home_ack_25 = false;   // :hC# accepted with :GE# 25 instead of 0
    bool park_zero_ok = false;  // :hP# accepted, reply reads '0', :GE# 00
    std::string ge_reply;       // non-empty: :GE# answers this instead of the code
    int brake_ms = 300;
    // Every driver timer and this fake's braking run on it (decision 0005).
    alpacacore::test::FakeTaskClock clock;
    alpacacore::util::TaskClock::clock::time_point stop_at{};
    bool stopping = false;
    int refuse_in_motion = 0;
    int refuse_code = 0;
    bool parked = false;
    bool has_ge = true;           // false: firmware without :GE# (answers like an unknown command)
    bool garbled_accept = false;  // :hP# accepted, but its reply reads as '0'
    bool silent = false;          // a mount that stops answering: every command times out
    int last_error = 0;
    // The fake time of the first :hP# or :hC#, and of every :hP#.
    std::optional<alpacacore::util::TaskClock::clock::time_point> first_try_at;
    std::vector<alpacacore::util::TaskClock::clock::time_point> park_tries;

    void settle_locked() {
        if (stopping && clock.now() - stop_at >= std::chrono::milliseconds(brake_ms)) {
            guide_active = false;
            goto_active = false;
            homing = false;
            pulse_active = false;
            stopping = false;
        }
    }
    bool g_locked() {
        settle_locked();
        return guide_active;
    }
    std::size_t count(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(std::count_if(
            log.begin(), log.end(), [&](const std::string& c) { return c.find(needle) != std::string::npos; }));
    }
    // Index of the first logged command containing `needle`, or log size.
    std::size_t first(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t i = 0; i < log.size(); ++i) {
            if (log[i].find(needle) != std::string::npos) return i;
        }
        return log.size();
    }
    // Refusal for :hP#/:hC#, or 0 when accepted.
    int refusal_locked() {
        if (g_locked() || goto_active || homing || pulse_active) return 22;
        if (refuse_in_motion > 0) {
            --refuse_in_motion;
            return 22;
        }
        return refuse_code;
    }
    std::string reply(const std::string& chunk) {
        std::lock_guard<std::mutex> lock(mutex);
        log.push_back(chunk);
        if (silent) return "";
        if (chunk.rfind(":GE", 0) == 0) {
            if (!has_ge) return "0";
            if (!ge_reply.empty()) return ge_reply;
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%02d#", last_error);
            return buf;
        }
        last_error = 0;
        if (chunk.rfind(":GVN", 0) == 0) return version + "#";
        if (chunk.rfind(":GXEE", 0) == 0) return "1";
        if (chunk.rfind(":GU", 0) == 0) {
            const bool g = g_locked();
            return std::string(tracking ? "" : "n") + (goto_active ? "" : "N") +
                   (parked ? "P" : (parking ? "I" : "p")) + (g && sends_g ? "g" : "") + (pulse_active ? "G" : "") +
                   (homing ? "h" : "") + "EW180#";
        }
        if (chunk.rfind(":Q", 0) == 0) {
            if ((guide_active || goto_active || homing || pulse_active) && !stopping) {
                stopping = true;
                stop_at = clock.now();
            }
            return "";
        }
        if (chunk.rfind(":MS", 0) == 0) {
            goto_active = true;
            return "0";  // GOTO accepted (bare digit)
        }
        if (chunk.rfind(":M", 0) == 0 || chunk.rfind(":R", 0) == 0) {
            guide_active = guide_active || chunk.rfind(":M", 0) == 0;
            return "";
        }
        if ((chunk.rfind(":hP", 0) == 0 || chunk.rfind(":hC", 0) == 0) && !first_try_at) {
            first_try_at = clock.now();
        }
        if (chunk.rfind(":hP", 0) == 0) {
            park_tries.push_back(clock.now());
            last_error = refusal_locked();
            if (last_error == 9) parked = true;  // "already parked": the mount is parked
            if (last_error != 0) return "0";
            last_error = park_zero_ok ? 0 : 25;
            parked = true;
            return garbled_accept || park_zero_ok ? "0" : "1";
        }
        if (chunk.rfind(":hC", 0) == 0) {
            last_error = refusal_locked();
            if (last_error == 0 && home_ack_25) last_error = 25;
            return "";
        }
        if (chunk.rfind(":S", 0) == 0) return "1";
        return "0#";
    }
};

std::unique_ptr<alpacacore::TelescopeDriver> connect_park_home_fake(alpacacore::test::FakeMountServer& server,
                                                                    ParkHomeFake& mount) {
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 200;
    return alpacacore::vendor::onstep::create_onstep_telescope(0, conn, mount.clock);
}

// A MoveAxis whose re-send body is already waiting on the fake clock.
void start_move(alpacacore::TelescopeDriver& driver, ParkHomeFake& mount, int axis, double rate) {
    const auto before = mount.clock.wait_count();
    driver.move_axis(axis, rate);
    REQUIRE(mount.clock.wait_for_wait_count(before + 1, std::chrono::seconds(5)));
}

// The background part of a Park or FindHome (#1430) runs on the fake clock:
// advance it in `step`s until Slewing reads false, so the stop, the settle
// wait and the retries run without a sleep for a driver timer (decision 0005
// rule 2). Slewing raises a refusal the body kept, which this rethrows.
// `waits_before` is mount.clock.wait_count() read before the Park or FindHome
// call. A MoveAxis re-send waiter can still sit on the clock after that call,
// so the rendezvous counts the body's own waits: step n goes out once wait
// number n after `waits_before` is registered.
void settle(ParkHomeFake& mount, alpacacore::TelescopeDriver& driver, std::uint64_t waits_before,
            std::chrono::milliseconds step = std::chrono::milliseconds(100)) {
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::uint64_t waits_wanted = waits_before;
    while (driver.get_slewing()) {
        REQUIRE(std::chrono::steady_clock::now() < give_up);
        // Let the body reach its next wait first, so no step lands while it
        // works. A body past its last wait (the mount still slews on the
        // clock) registers none: advance after the bound, and count from the
        // waits seen so far.
        if (mount.clock.wait_for_wait_count(waits_wanted + 1, std::chrono::milliseconds(200))) {
            ++waits_wanted;
        } else {
            waits_wanted = mount.clock.wait_count();
        }
        mount.clock.advance(step);
        REQUIRE(mount.clock.wait_for_woken_settled(std::chrono::seconds(5)));
    }
}

// A Park or FindHome as a client sees it: the call (which throws a refusal it
// can decide at once), then Slewing until the background part ends.
void on_clock(ParkHomeFake& mount, alpacacore::TelescopeDriver& driver, const std::function<void()>& call,
              std::chrono::milliseconds step = std::chrono::milliseconds(100)) {
    const auto waits_before = mount.clock.wait_count();
    call();
    settle(mount, driver, waits_before, step);
}

}  // namespace

// #1430 (HM-17, 2026-10-20): MoveAxis(1, 0) and Park at once returned success
// and the mount never parked: OnStepX refuses a park while the axis brakes.
// Park now stops any motion, waits until the mount shows none, and parks.
TEST_CASE("OnStep Telescope Driver - Park during a MoveAxis stops the move and parks", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    start_move(*driver, *mount, 0, 1.0);
    const auto t0 = mount->clock.now();
    REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->park(); }));
    REQUIRE(mount->first_try_at);
    // Braking ends at 300 ms; with 100 ms polls the park goes out at the first
    // poll that finds the mount still.
    CHECK(mount->first_try_at.value_or(t0 + std::chrono::hours(1)) - t0 < std::chrono::milliseconds(400));
    CHECK(mount->first(":Qe#") < mount->first(":hP#"));  // stopped before the park
    CHECK(mount->count(":hP#") >= 1);
    const auto moves = mount->count(":Me#");
    mount->clock.advance(std::chrono::seconds(7));  // past the re-send interval
    (void)mount->clock.wait_for_woken_settled(std::chrono::seconds(5));
    CHECK(mount->count(":Me#") == moves);  // the re-send ended with the move

    driver->set_connected(false);
}

TEST_CASE("OnStep Telescope Driver - Park is retried while the mount reports it still in motion",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->refuse_in_motion = 2;
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->park(); }));
    CHECK(mount->count(":hP#") == 3);
    CHECK(mount->count(":GE#") == 2);
    std::lock_guard<std::mutex> lock(mount->mutex);
    REQUIRE(mount->park_tries.size() == 3);
    for (std::size_t i = 1; i < mount->park_tries.size(); ++i) {
        CHECK(mount->park_tries[i] - mount->park_tries[i - 1] >= std::chrono::milliseconds(200));  // paced retries
    }

    driver->set_connected(false);
}

// #1430: a :hP# reply that reads as '0' while :GE# says the park was taken
// (25) is a park in progress, not a refusal: no error, and no second :hP#,
// which OnStepX would refuse while parking.
TEST_CASE("OnStep Telescope Driver - Park taken by the mount despite a garbled reply succeeds",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->garbled_accept = true;
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->park(); }));
    CHECK(mount->count(":hP#") == 1);
    CHECK(mount->count(":GE#") == 1);

    driver->set_connected(false);
}

// The 6 s settle and the 3 s of retries run on the task clock: a mount that
// brakes for a minute ends the Park after about 9 s of fake time. A deadline
// read from the real clock would let fake time run far past that.
TEST_CASE("OnStep Telescope Driver - Park gives up when the mount does not stop in time", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->brake_ms = 60000;
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    start_move(*driver, *mount, 0, 1.0);
    const auto t0 = mount->clock.now();
    try {
        on_clock(*mount, *driver, [&] { driver->park(); });
        FAIL("Park returned success although the mount never stopped");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
        CHECK(std::string(ex.what()).find("in motion") != std::string::npos);
    }
    const auto elapsed = mount->clock.now() - t0;
    CHECK(elapsed >= std::chrono::seconds(9));
    CHECK(elapsed < std::chrono::seconds(10));
    CHECK(mount->count(":hP#") >= 1);

    driver->set_connected(false);
}

TEST_CASE("OnStep Telescope Driver - Park reports why the mount refused it", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->refuse_code = 12;  // CE_NO_PARK_POSITION_SET
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    try {
        on_clock(*mount, *driver, [&] { driver->park(); });
        FAIL("Park returned success although the mount refused it");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
        CHECK(std::string(ex.what()).find("no park position set") != std::string::npos);
    }
    CHECK(mount->count(":hP#") == 1);  // not a motion refusal: no retry
    CHECK(mount->count(":Q") == 0);    // a still mount gets no stops

    driver->set_connected(false);
}

TEST_CASE("OnStep Telescope Driver - FindHome reports why the mount refused it", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->refuse_code = 20;  // CE_SLEW_ERR_OUTSIDE_LIMITS
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    try {
        on_clock(*mount, *driver, [&] { driver->find_home(); });
        FAIL("FindHome returned success although the mount refused it");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
        CHECK(std::string(ex.what()).find("outside configured limits") != std::string::npos);
    }

    driver->set_connected(false);
}

TEST_CASE("OnStep Telescope Driver - FindHome during a MoveAxis stops the move and homes",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    start_move(*driver, *mount, 1, 1.0);
    const auto t0 = mount->clock.now();
    REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->find_home(); }));
    REQUIRE(mount->first_try_at);
    CHECK(mount->first_try_at.value_or(t0 + std::chrono::hours(1)) - t0 < std::chrono::seconds(1));
    CHECK(mount->first(":Qn#") < mount->first(":hC#"));
    CHECK(mount->count(":hC#") >= 1);
    const auto moves = mount->count(":Mn#");
    mount->clock.advance(std::chrono::seconds(7));  // past the re-send interval
    (void)mount->clock.wait_for_woken_settled(std::chrono::seconds(5));
    CHECK(mount->count(":Mn#") == moves);  // the re-send ended with the move

    driver->set_connected(false);
}

// #1430: a Park while one runs ('I') and a FindHome while one runs ('h') are
// no-ops: nothing is stopped or sent.
TEST_CASE("OnStep Telescope Driver - Park while parking and FindHome while homing send nothing",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    {
        std::lock_guard<std::mutex> lock(mount->mutex);
        mount->parking = true;
    }
    REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->park(); }));
    CHECK(mount->count(":hP#") == 0);
    CHECK(mount->count(":Q") == 0);
    {
        std::lock_guard<std::mutex> lock(mount->mutex);
        mount->parking = false;
        mount->homing = true;
    }
    REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->find_home(); }));
    CHECK(mount->count(":hC#") == 0);
    CHECK(mount->count(":Q") == 0);

    driver->set_connected(false);
}

// #1430: OnStepX refuses a park during a GOTO; Park stops the GOTO first.
TEST_CASE("OnStep Telescope Driver - Park during a GOTO stops the GOTO and parks", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    {
        std::lock_guard<std::mutex> lock(mount->mutex);
        mount->goto_active = true;
    }
    REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->park(); }));
    CHECK(mount->first(":Q#") < mount->first(":hP#"));
    CHECK(mount->count(":hP#") >= 1);

    driver->set_connected(false);
}

// #1430 (review): OnStepX also refuses a park while a home ('h') or a pulse
// guide ('G') runs (Park::request: guide.state != GU_NONE, a home running), so
// Park stops those first too.
TEST_CASE("OnStep Telescope Driver - Park during a home stops the home and parks", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    {
        std::lock_guard<std::mutex> lock(mount->mutex);
        mount->homing = true;
    }
    REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->park(); }));
    CHECK(mount->first(":Q#") < mount->first(":hP#"));
    CHECK(mount->count(":hP#") == 1);  // accepted at once: the home had ended

    driver->set_connected(false);
}

// #1478: the :Mg[nsew]#### field holds 4 digits, so a longer PulseGuide is
// refused (InvalidValue) instead of being clamped to 9999 ms and reported as
// done; 9999 ms itself still goes out.
TEST_CASE("OnStep Telescope Driver - PulseGuide refuses a duration over 9999 ms", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    require_alpaca_error([&] { driver->pulse_guide(0, 10000); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(mount->count(":Mg") == 0);
    CHECK_FALSE(driver->get_is_pulse_guiding());
    REQUIRE_NOTHROW(driver->pulse_guide(0, 9999));
    CHECK(mount->count(":Mgn9999#") == 1);
    driver->set_connected(false);
}

TEST_CASE("OnStep Telescope Driver - Park during a pulse guide stops it and parks", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    {
        std::lock_guard<std::mutex> lock(mount->mutex);
        mount->pulse_active = true;
    }
    REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->park(); }));
    CHECK(mount->first(":Q#") < mount->first(":hP#"));
    CHECK(mount->count(":hP#") == 1);  // accepted at once: the pulse had ended

    driver->set_connected(false);
}

// #1430: what the mount accepts. A home with no :GE# (firmware without it)
// or with 25 counts as started, and a park whose reply reads '0' counts as
// taken when :GE# says 00.
TEST_CASE("OnStep Telescope Driver - Park and FindHome accepted without a clean reply succeed",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    SECTION("FindHome on firmware without :GE#") {
        mount->has_ge = false;
        // 1 s steps: the :GE# read waits out its 1 s timeout in real time.
        REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->find_home(); }, std::chrono::seconds(1)));
        CHECK(mount->count(":hC#") == 1);
    }
    SECTION("FindHome whose :GE# reply is not a two-digit code") {
        const auto reply = GENERATE(std::string("E1#"), std::string("1E#"), std::string("123#"));
        mount->ge_reply = reply;
        REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->find_home(); }));
        CHECK(mount->count(":hC#") == 1);
    }
    SECTION("FindHome answered 25") {
        mount->home_ack_25 = true;
        REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->find_home(); }));
        CHECK(mount->count(":hC#") == 1);
    }
    SECTION("Park answered 0 with :GE# 00") {
        mount->park_zero_ok = true;
        REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->park(); }));
        CHECK(mount->count(":hP#") == 1);
    }
    driver->set_connected(false);
}

// #1430: on firmware without 'g' only the driver knows a MoveAxis runs, so
// Park and FindHome stop it from the driver's own flags.
TEST_CASE("OnStep Telescope Driver - Park and FindHome stop a MoveAxis on firmware without 'g'",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->sends_g = false;
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    SECTION("Park, axis 0") {
        start_move(*driver, *mount, 0, 1.0);
        REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->park(); }));
        CHECK(mount->first(":Qe#") < mount->first(":hP#"));
    }
    SECTION("FindHome, axis 1") {
        start_move(*driver, *mount, 1, 1.0);
        REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->find_home(); }));
        CHECK(mount->first(":Qn#") < mount->first(":hC#"));
    }
    driver->set_connected(false);
}

// #1430: a Park that stopped a GOTO drops the tracking restore that GOTO
// carried, and Slewing reads false at once, even when the park is then
// refused: an AbortSlew afterwards must not restart tracking.
TEST_CASE("OnStep Telescope Driver - a GOTO stopped by Park is not resumed as a slew or a tracking restore",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    driver->slew_to_coordinates_async(12.0, 45.0);
    {
        std::lock_guard<std::mutex> lock(mount->mutex);
        mount->refuse_code = 12;  // CE_NO_PARK_POSITION_SET, once the GOTO is stopped
        mount->tracking = false;  // the stopped GOTO left tracking off
    }
    CHECK_THROWS_AS(on_clock(*mount, *driver, [&] { driver->park(); }), alpacacore::AlpacaException);
    CHECK(mount->first(":Q#") < mount->first(":hP#"));
    REQUIRE_NOTHROW(driver->abort_slew());  // drops the kept refusal
    CHECK_FALSE(driver->get_slewing());     // inside the 2 s slew grace: the stop cleared it
    CHECK(mount->count(":ST6") == 0);       // no tracking restore (:ST60.164275#) for the stopped GOTO

    driver->set_connected(false);
}

// #1430: a stop that cannot be sent fails the Park with DriverException
// before :hP#, as MoveAxis(axis, 0) and AbortSlew report theirs (#742,
// #781). The stops are blind sends, so the fake drops the connection: every
// write after that fails while the wrapper still reads the link as alive.
TEST_CASE("OnStep Telescope Driver - Park reports a stop it could not send", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    start_move(*driver, *mount, 0, 1.0);
    REQUIRE(server.drop_connections());
    try {
        on_clock(*mount, *driver, [&] { driver->park(); });
        FAIL("Park returned success although the motion could not be stopped");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("could not stop the running motion first") != std::string::npos);
    }
    CHECK(mount->count(":hP#") == 0);
    // The re-send ends with the failed stop, as AbortSlew's does (#781): no
    // tick runs, so no status read fails on the dropped link.
    std::atomic<int> resend_warns{0};
    {
        struct SinkGuard {
            alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
            ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
        } sink_guard;
        alpacacore::logging::set_log_sink(
            [&](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn && component == "OnStep" &&
                    message.find("MoveAxis re-send") != std::string_view::npos) {
                    ++resend_warns;
                }
            });
        mount->clock.advance(std::chrono::seconds(14));  // two re-send intervals
        (void)mount->clock.wait_for_woken_settled(std::chrono::seconds(5));
    }
    CHECK(resend_warns.load() == 0);
    driver->set_connected(false);
}

// #1430: each OnStepX refusal code reads as its reason (COMMAND_REFERENCE.md,
// "Command Error Codes"), and an unknown one as its number. 9 ("already
// parked") is a no-op instead (next case).
TEST_CASE("OnStep Telescope Driver - Park names every OnStepX refusal code", "[onstep][telescope][unit]") {
    const auto [code, reason] = GENERATE(table<int, std::string>({
        {8, "not parked or at home"},
        {10, "park failed"},
        {11, "not parked"},
        {12, "no park position set"},
        {13, "goto failed"},
        {15, "target below the horizon limit"},
        {16, "target above the overhead limit"},
        {17, "controller in standby"},
        {18, "mount parked"},
        {19, "goto already active"},
        {20, "outside configured limits"},
        {21, "hardware fault"},
        {23, "slew error"},
        {14, "error code 14"},
    }));
    auto mount = std::make_shared<ParkHomeFake>();
    mount->refuse_code = code;
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    try {
        on_clock(*mount, *driver, [&] { driver->park(); });
        FAIL("Park returned success although the mount refused it");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
        CHECK(std::string(ex.what()).find("(" + reason + ")") != std::string::npos);
    }
    driver->set_connected(false);
}

// A Park that stops a running GOTO leaves the mount braking for a moment
// (OnStepX keeps the GOTO state about 1.6 s after :Q# on the HM-17). A slew
// sent then is refused as "GOTO running", as any slew during a GOTO is
// (#1379), and the Park goes on: the slew does not supersede it.
TEST_CASE(
    "OnStep Telescope Driver - a slew while a Park still brakes the GOTO it stopped is refused and the Park "
    "goes on",
    "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(2.0, 10.0));  // the fake runs the GOTO
    const auto waits_before = mount->clock.wait_count();
    REQUIRE_NOTHROW(driver->park());  // stops the GOTO; the body waits for the mount to settle
    try {
        driver->slew_to_coordinates_async(3.0, 20.0);
        FAIL("a slew went out while the stopped GOTO still braked");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
        CHECK(std::string(ex.what()) == "SlewToCoordinatesAsync is not allowed while a GOTO is running");
    }
    CHECK(mount->count(":MS#") == 1);  // only the first GOTO
    settle(*mount, *driver, waits_before);
    CHECK(mount->count(":hP#") >= 1);  // the Park still went out
    CHECK(driver->get_at_park());
    driver->set_connected(false);
}

// OnStep 4.x answers a park of a parked mount with 9 ("already parked",
// Park.ino park()) and a home of one with 18 ("mount parked", Home.ino
// goHome() -> validateGoto()); the driver meets them when its status cache
// still read the mount unparked. ASCOM: Park on a parked mount is a no-op,
// and FindHome while parked is InvalidWhileParked.
TEST_CASE(
    "OnStep Telescope Driver - a park answered already parked is a no-op and a home answered parked is "
    "InvalidWhileParked",
    "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    SECTION("Park answered 9") {
        mount->refuse_code = 9;
        REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->park(); }));
        CHECK(mount->count(":hP#") == 1);  // no retry
        CHECK(driver->get_at_park());
        CHECK_FALSE(driver->get_slewing());
    }
    SECTION("FindHome answered 18") {
        mount->refuse_code = 18;
        try {
            driver->find_home();
            FAIL("FindHome succeeded on a parked mount");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidWhileParked);
        }
    }
    driver->set_connected(false);
}

// A no-op Park or FindHome is a new command: it drops the refusal an earlier
// body kept, as any other new command does (the shared async rule), so
// Slewing no longer raises it.
TEST_CASE("OnStep Telescope Driver - a no-op Park or FindHome drops an earlier body's kept refusal",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    start_move(*driver, *mount, 0, 1.0);  // so the park runs in the body
    mount->refuse_code = 21;              // the body's try is refused: hardware fault
    CHECK_THROWS_AS(on_clock(*mount, *driver, [&] { driver->park(); }), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_slewing(), alpacacore::AlpacaException);  // the kept refusal

    SECTION("the mount was parked meanwhile, then Park") {
        {
            std::lock_guard<std::mutex> lock(mount->mutex);
            mount->parked = true;  // the handset parked it
        }
        mount->clock.advance(std::chrono::milliseconds(2100));  // past the status cache
        REQUIRE_NOTHROW(driver->park());
    }
    SECTION("the mount was homed meanwhile, then FindHome") {
        {
            std::lock_guard<std::mutex> lock(mount->mutex);
            mount->homing = true;  // on its way home
        }
        mount->clock.advance(std::chrono::milliseconds(2100));
        REQUIRE_NOTHROW(driver->find_home());
    }
    CHECK_NOTHROW((void)driver->get_slewing());
    driver->set_connected(false);
}

// A home with home sensors runs as a guide move (:GU# 'h' with 'N'), not a
// GOTO, so the GOTO-running check missed it: a slew went out, ended the
// waiting FindHome and met the mount's "already moving" (8).
TEST_CASE("OnStep Telescope Driver - a slew or sync while the mount homes is refused", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    {
        std::lock_guard<std::mutex> lock(mount->mutex);
        mount->homing = true;
    }
    mount->clock.advance(std::chrono::milliseconds(2100));  // past the status cache
    const auto stops_before = mount->count(":Q");
    const auto require_refused = [](const std::function<void()>& fn, const std::string& who) {
        try {
            fn();
            FAIL(who + " went out while the mount homes");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
            CHECK(std::string(ex.what()) == who + " is not allowed while the mount is homing");
        }
    };
    require_refused([&] { driver->slew_to_coordinates_async(2.0, 10.0); }, "SlewToCoordinatesAsync");
    require_refused([&] { driver->sync_to_coordinates(2.0, 10.0); }, "SyncToCoordinates");
    CHECK(mount->count(":MS#") == 0);
    CHECK(mount->count(":CM#") == 0);
    CHECK(mount->count(":Q") == stops_before);  // the home was not stopped
    driver->set_connected(false);
}

// OnStepX :Q# ends every guide (Guide::stop()) and :Qe#/:Qw#, :Qn#/:Qs# end
// the guides on one axis, a pulse guide included; IsPulseGuiding read true
// until the pulse's own end time after them.
TEST_CASE("OnStep Telescope Driver - AbortSlew and a MoveAxis stop on its axis end IsPulseGuiding",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    driver->pulse_guide(0, 5000);  // North: axis 1
    REQUIRE(driver->get_is_pulse_guiding());

    SECTION("AbortSlew") {
        driver->abort_slew();
        CHECK_FALSE(driver->get_is_pulse_guiding());
    }
    SECTION("MoveAxis(1, 0)") {
        driver->move_axis(1, 0.0);
        CHECK_FALSE(driver->get_is_pulse_guiding());
    }
    SECTION("MoveAxis(0, 0) leaves a North pulse") {
        driver->move_axis(0, 0.0);
        CHECK(driver->get_is_pulse_guiding());
    }
    driver->set_connected(false);
}

namespace {

// Moves the fake clock `total` in 100 ms steps, letting each woken task run.
void run_clock(ParkHomeFake& mount, std::chrono::milliseconds total) {
    for (auto t = std::chrono::milliseconds(0); t < total; t += std::chrono::milliseconds(100)) {
        (void)mount.clock.wait_for_waiters(1, std::chrono::milliseconds(200));  // a body reaches its wait first
        mount.clock.advance(std::chrono::milliseconds(100));
        (void)mount.clock.wait_for_woken_settled(std::chrono::seconds(5));
    }
}

}  // namespace

// #1430: Park and FindHome are asynchronous initiators (the 1 s STANDARD
// target): they send the stops and return; the wait for the mount to be still
// and the park or home itself run in the background, with Slewing true.
TEST_CASE("OnStep Telescope Driver - Park and FindHome return at once while they stop a motion",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->brake_ms = 3000;
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    start_move(*driver, *mount, 0, 1.0);
    const auto t0 = mount->clock.now();
    const auto call = GENERATE(0, 1);
    const char* const sent = call == 0 ? ":hP#" : ":hC#";
    const auto waits_before = mount->clock.wait_count();
    if (call == 0) {
        REQUIRE_NOTHROW(driver->park());
    } else {
        REQUIRE_NOTHROW(driver->find_home());
    }
    CHECK(mount->clock.now() == t0);  // no wait in the call
    CHECK(mount->count(":Qe#") >= 1);
    CHECK(mount->count(sent) == 0);  // the mount still brakes
    CHECK(driver->get_slewing());    // the background part runs
    CHECK_NOTHROW(driver->get_at_park());
    settle(*mount, *driver, waits_before);
    CHECK(mount->count(sent) == 1);
    driver->set_connected(false);
}

// #1430: a newer command ends a Park or FindHome still finishing in the
// background, before or between its tries; nothing more is sent for it.
TEST_CASE("OnStep Telescope Driver - a newer command ends a Park still finishing", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->brake_ms = 3000;
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    start_move(*driver, *mount, 0, 1.0);
    REQUIRE_NOTHROW(driver->park());
    REQUIRE(mount->count(":hP#") == 0);

    SECTION("AbortSlew") { REQUIRE_NOTHROW(driver->abort_slew()); }
    SECTION("a GOTO") { REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0)); }
    SECTION("Unpark") { REQUIRE_NOTHROW(driver->unpark()); }
    SECTION("a MoveAxis") { REQUIRE_NOTHROW(driver->move_axis(1, 1.0)); }
    SECTION("a FindHome") {
        REQUIRE_NOTHROW(driver->find_home());
        run_clock(*mount, std::chrono::seconds(10));
        CHECK(mount->count(":hC#") == 1);  // the newer one runs
    }
    SECTION("a disconnect") { driver->set_connected(false); }
    SECTION("a reconnect") {
        driver->set_connected(false);
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
    }
    run_clock(*mount, std::chrono::seconds(10));
    CHECK(mount->count(":hP#") == 0);
    driver->set_connected(false);
}

// #1430, decision 0006 rule 3: Park claims its background part under the
// driver lock, before it releases it. A newer command that lands between the
// release and the task's start (another client's AbortSlew) still ends it.
TEST_CASE("OnStep Telescope Driver - a newer command before Park's background part starts still ends it",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->brake_ms = 3000;
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    start_move(*driver, *mount, 0, 1.0);
    alpacacore::vendor::onstep::set_park_home_before_start_for_testing(*driver, [&] { driver->abort_slew(); });

    REQUIRE_NOTHROW(driver->park());
    alpacacore::vendor::onstep::set_park_home_before_start_for_testing(*driver, nullptr);
    run_clock(*mount, std::chrono::seconds(10));
    CHECK(mount->count(":hP#") == 0);  // the AbortSlew won
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// #1430: a newer FindHome whose start() lands before the older Park's still
// homes: the body started last runs the newer request.
TEST_CASE("OnStep Telescope Driver - a FindHome started before an older Park's background part still homes",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->brake_ms = 3000;
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    start_move(*driver, *mount, 0, 1.0);
    alpacacore::vendor::onstep::set_park_home_before_start_for_testing(*driver, [&] {
        alpacacore::vendor::onstep::set_park_home_before_start_for_testing(*driver, nullptr);
        driver->find_home();  // runs to its own start() inside the Park's gap
    });

    REQUIRE_NOTHROW(driver->park());
    REQUIRE(driver->get_slewing());
    run_clock(*mount, std::chrono::seconds(10));
    CHECK(mount->count(":hC#") == 1);  // the FindHome won
    CHECK(mount->count(":hP#") == 0);
    driver->set_connected(false);
}

// #1430: a Park body whose thread starts late, after a newer FindHome
// cancelled it and stored its own request but before that FindHome's start()
// marked it superseded, must leave that request alone. It read the request,
// saw itself stopped and cleared it as done, so FindHome reported success
// and nothing homed (about 1 run in 100 of "a newer command ends a Park
// still finishing"). The spawn seam holds the Park's thread until the gap.
TEST_CASE("OnStep Telescope Driver - a Park body that starts late leaves a newer FindHome to run",
          "[onstep][telescope][unit]") {
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool released = false;
    bool park_body_returned = false;
    std::atomic<int> spawns{0};
    auto mount = std::make_shared<ParkHomeFake>();
    mount->brake_ms = 3000;
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    start_move(*driver, *mount, 0, 1.0);
    alpacacore::vendor::onstep::set_park_home_spawn_for_testing(
        *driver, [&](std::function<void()> body) -> std::thread {
            if (spawns.fetch_add(1) != 0) {
                return std::thread(std::move(body));
            }
            return std::thread([&, body = std::move(body)] {  // the Park's body: held until released
                {
                    std::unique_lock<std::mutex> lock(gate_mutex);
                    // Bounded, so a case that fails before the release ends.
                    gate_cv.wait_for(lock, std::chrono::seconds(10), [&] { return released; });
                }
                body();
                {
                    std::lock_guard<std::mutex> lock(gate_mutex);
                    park_body_returned = true;
                }
                gate_cv.notify_all();
            });
        });

    REQUIRE_NOTHROW(driver->park());  // its body is held
    // In FindHome's gap (its request stored, its start() not yet called):
    // let the held Park body run to its end.
    alpacacore::vendor::onstep::set_park_home_before_start_for_testing(*driver, [&] {
        std::unique_lock<std::mutex> lock(gate_mutex);
        released = true;
        gate_cv.notify_all();
        gate_cv.wait_for(lock, std::chrono::seconds(5), [&] { return park_body_returned; });
    });
    const auto waits_before = mount->clock.wait_count();
    REQUIRE_NOTHROW(driver->find_home());
    alpacacore::vendor::onstep::set_park_home_before_start_for_testing(*driver, nullptr);
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        REQUIRE(park_body_returned);
    }
    settle(*mount, *driver, waits_before);
    CHECK(mount->count(":hC#") == 1);  // the FindHome still runs
    CHECK(mount->count(":hP#") == 0);
    driver->set_connected(false);
}

// #1626 (from the #1564 review): a MoveAxis, slew or Unpark refused for a
// latched link fault is refused before it supersedes a waiting Park, so the
// Park still runs once the mount answers. The spawn seam holds the Park's body
// while the fault latches and the command is refused.
TEST_CASE("OnStep Telescope Driver - a command refused for a link fault leaves a waiting Park to run",
          "[onstep][telescope][unit]") {
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool released = false;
    bool park_body_returned = false;
    auto mount = std::make_shared<ParkHomeFake>();
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    start_move(*driver, *mount, 0, 1.0);  // so Park stops first and its body waits for the settle
    alpacacore::vendor::onstep::set_park_home_spawn_for_testing(
        *driver, [&](std::function<void()> body) -> std::thread {
            return std::thread([&, body = std::move(body)] {  // held until released
                {
                    std::unique_lock<std::mutex> lock(gate_mutex);
                    // Bounded, so a case that fails before the release ends.
                    gate_cv.wait_for(lock, std::chrono::seconds(10), [&] { return released; });
                }
                body();
                {
                    std::lock_guard<std::mutex> lock(gate_mutex);
                    park_body_returned = true;
                }
                gate_cv.notify_all();
            });
        });
    REQUIRE_NOTHROW(driver->park());  // its body is held

    {
        std::lock_guard<std::mutex> lock(mount->mutex);
        mount->silent = true;
    }
    mount->clock.advance(std::chrono::milliseconds(2100));  // past the 2 s caches
    for (int i = 0; i < 3; ++i) {
        try {
            (void)driver->get_right_ascension();
        } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch)
            // the third throws: the fault latched
        }
    }
    REQUIRE_FALSE(driver->get_link_fault().empty());
    const auto require_refused = [](const std::function<void()>& fn, const std::string& what) {
        try {
            fn();
            FAIL(what + " succeeded during a fault");
        } catch (const alpacacore::AlpacaException& ex) {
            INFO(ex.what());
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).rfind(what + ": ", 0) == 0);
        }
    };
    SECTION("MoveAxis") {
        require_refused([&] { driver->move_axis(1, 1.0); }, "MoveAxis");
    }
    SECTION("SlewToCoordinatesAsync") {
        require_refused([&] { driver->slew_to_coordinates_async(2.0, 10.0); }, "SlewToCoordinatesAsync");
    }
    SECTION("Unpark") {
        require_refused([&] { driver->unpark(); }, "Unpark");
    }

    {
        std::lock_guard<std::mutex> lock(mount->mutex);
        mount->silent = false;
    }
    mount->clock.advance(std::chrono::milliseconds(2100));  // past the faulted-read back-off
    const auto waits_before = mount->clock.wait_count();
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        released = true;
    }
    gate_cv.notify_all();
    settle(*mount, *driver, waits_before, std::chrono::milliseconds(500));
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(gate_cv.wait_for(lock, std::chrono::seconds(5), [&] { return park_body_returned; }));
    }
    CHECK(mount->count(":hP#") == 1);  // the Park still ran
    CHECK(driver->get_at_park());
    CHECK(driver->get_link_fault().empty());
    driver->set_connected(false);
}

TEST_CASE("OnStep Telescope Driver - an AbortSlew between Park's in-motion retries ends it",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->refuse_in_motion = 1000;  // a still mount that keeps answering 22
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    REQUIRE_NOTHROW(driver->park());  // the first try, refused "in motion", then the background retries
    REQUIRE(mount->count(":hP#") == 1);
    REQUIRE_NOTHROW(driver->abort_slew());
    run_clock(*mount, std::chrono::seconds(5));
    CHECK(mount->count(":hP#") == 1);
    CHECK_NOTHROW(driver->get_slewing());  // ended, not refused
    driver->set_connected(false);
}

// #1430: a FindHome the mount takes at once also ends a Park still waiting
// to retry, although it starts no background part of its own.
TEST_CASE("OnStep Telescope Driver - a FindHome taken at once ends a Park still retrying",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->refuse_in_motion = 1;  // the Park's first try only
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    REQUIRE_NOTHROW(driver->park());  // refused "in motion": the retries go to the background
    REQUIRE(mount->count(":hP#") == 1);
    REQUIRE_NOTHROW(driver->find_home());  // taken at once
    REQUIRE(mount->count(":hC#") == 1);
    run_clock(*mount, std::chrono::seconds(5));
    CHECK(mount->count(":hP#") == 1);
    driver->set_connected(false);
}

// #1430, the shared rule "A failed async operation body keeps its error": a
// refusal the background part meets is kept and raised by Slewing and AtPark
// (a park's) until the next motion command or a reconnect, each a clear site
// of its own.
TEST_CASE("OnStep Telescope Driver - a Park refusal met in the background is kept until the next motion command",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->refuse_code = 12;  // CE_NO_PARK_POSITION_SET
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    start_move(*driver, *mount, 0, 1.0);
    const auto waits_before = mount->clock.wait_count();
    REQUIRE_NOTHROW(driver->park());  // stops, then refused in the background
    const auto require_refusal = [](const std::function<void()>& fn) {
        try {
            fn();
            FAIL("the kept refusal was not raised");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
            CHECK(std::string(ex.what()).find("no park position set") != std::string::npos);
        }
    };
    require_refusal([&] { settle(*mount, *driver, waits_before); });
    require_refusal([&] { (void)driver->get_slewing(); });  // still kept
    require_refusal([&] { (void)driver->get_at_park(); });
    CHECK_NOTHROW(driver->get_at_home());  // a park's refusal only
    {
        std::lock_guard<std::mutex> lock(mount->mutex);
        mount->refuse_code = 0;
    }

    SECTION("AbortSlew clears it") { REQUIRE_NOTHROW(driver->abort_slew()); }
    SECTION("a MoveAxis clears it") { REQUIRE_NOTHROW(driver->move_axis(1, 1.0)); }
    SECTION("a GOTO clears it") { REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0)); }
    SECTION("Unpark clears it") { REQUIRE_NOTHROW(driver->unpark()); }
    SECTION("FindHome clears it") { REQUIRE_NOTHROW(driver->find_home()); }
    SECTION("Park clears it") { REQUIRE_NOTHROW(driver->park()); }
    SECTION("a reconnect clears it") {
        driver->set_connected(false);
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
    }
    CHECK_NOTHROW(driver->get_slewing());
    CHECK_NOTHROW(driver->get_at_park());
    driver->set_connected(false);
}

TEST_CASE("OnStep Telescope Driver - a FindHome refusal met in the background is raised by AtHome only",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->refuse_code = 20;  // CE_SLEW_ERR_OUTSIDE_LIMITS
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    start_move(*driver, *mount, 1, 1.0);
    const auto waits_before = mount->clock.wait_count();
    REQUIRE_NOTHROW(driver->find_home());
    CHECK_THROWS_AS(settle(*mount, *driver, waits_before), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_at_home(), alpacacore::AlpacaException);
    CHECK_NOTHROW(driver->get_at_park());
    driver->set_connected(false);
}

TEST_CASE("OnStep Telescope Driver - FindHome is retried while the mount reports it still in motion",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->refuse_in_motion = 2;
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    REQUIRE_NOTHROW(on_clock(*mount, *driver, [&] { driver->find_home(); }));
    CHECK(mount->count(":hC#") == 3);

    driver->set_connected(false);
}

// OnStep 4.x has :GE# with the same codes (OnStep Globals.h CommandErrors):
// the reason is reported as on OnStepX.
TEST_CASE("OnStep Telescope Driver - Park refused by OnStep 4.x reports the reason", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->version = "4.24j";
    mount->refuse_code = 10;  // CE_PARK_FAILED
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    try {
        on_clock(*mount, *driver, [&] { driver->park(); });
        FAIL("Park returned success although the mount refused it");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
        CHECK(std::string(ex.what()).find("park failed") != std::string::npos);
    }
    CHECK(mount->count(":hP#") == 1);

    driver->set_connected(false);
}

// Firmware without :GE#: a refused park is retried for the window, then
// reported without a reason.
TEST_CASE("OnStep Telescope Driver - Park refused by firmware without :GE# is reported without a reason",
          "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ParkHomeFake>();
    mount->version = "3.16o";
    mount->has_ge = false;
    mount->refuse_code = 10;
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    auto driver = connect_park_home_fake(server, *mount);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    try {
        // 1 s steps: each try also waits out the 1 s :GE# timeout in real time.
        on_clock(*mount, *driver, [&] { driver->park(); }, std::chrono::seconds(1));
        FAIL("Park returned success although the mount refused it");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
        CHECK(std::string(ex.what()).find("no reason given") != std::string::npos);
    }
    CHECK(mount->count(":hP#") >= 2);  // retried: the reason is unknown

    driver->set_connected(false);
}

TEST_CASE("OnStep Telescope Driver - FindHome and Park never restart tracking", "[onstep][telescope][unit]") {
    alpacacore::test::FakeTaskClock clock;  // every driver timer (decision 0005); outlives the driver
    auto mount = std::make_shared<ScriptedOnStep>();
    mount->set_status("NpEW180#");  // tracking, not at home or parked
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 200;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    const auto finish_home = [&]() {
        mount->set_status("npHhEo180#");  // homing
        observe_slewing(*driver, *mount, clock);
        mount->clear_log();
        mount->set_status("nNpHEo180#");  // at home, not tracking (HM-17 trace, #1218)
        REQUIRE(wait_slew_done(*driver, clock));
        CHECK(driver->get_at_home());
        CHECK(mount->count(":ST6") == 0);
        CHECK(mount->count(":Te#") == 0);
    };
    // A GOTO whose end the driver never saw, so its tracking flag is still set.
    const auto abort_a_goto = [&]() {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0));
        REQUIRE_NOTHROW(driver->abort_slew());
        mount->set_status("NpEW180#");
    };

    SECTION("FindHome after an aborted GOTO") {
        abort_a_goto();
        REQUIRE_NOTHROW(driver->find_home());
        finish_home();
    }

    SECTION("Park from home reads as slewing and away from home at once") {
        mount->set_status("NpHEW180#");  // tracking, at home
        REQUIRE_NOTHROW(driver->park());
        CHECK(driver->get_slewing());
        CHECK_FALSE(driver->get_at_home());
    }

    SECTION("Park after an aborted GOTO") {
        abort_a_goto();
        REQUIRE_NOTHROW(driver->park());
        mount->set_status("nIEW180#");  // parking
        observe_slewing(*driver, *mount, clock);
        mount->clear_log();
        mount->set_status("nNPHEo180#");  // parked
        REQUIRE(wait_slew_done(*driver, clock));
        CHECK(driver->get_at_park());
        CHECK(mount->count(":ST6") == 0);
        CHECK(mount->count(":Te#") == 0);
    }

    driver->set_connected(false);
}

TEST_CASE("OnStep Telescope Driver - a coordinate slew restores tracking only if the mount lost it",
          "[onstep][telescope][unit]") {
    alpacacore::test::FakeTaskClock clock;  // every driver timer (decision 0005); outlives the driver
    auto mount = std::make_shared<ScriptedOnStep>();
    mount->set_status("NpEW180#");  // tracking before the slew
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 200;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    REQUIRE(driver->get_tracking());

    SECTION("tracking kept through the GOTO: nothing is sent") {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0));
        mount->set_status("pEW180#");  // slewing, tracking
        observe_slewing(*driver, *mount, clock);
        mount->clear_log();
        mount->set_status("NpEW180#");  // done, still tracking (OnStepX)
        REQUIRE(wait_slew_done(*driver, clock));
        CHECK(mount->count(":ST6") == 0);
    }

    SECTION("a slew already running at connect is left alone") {
        mount->set_status("npEW180#");  // slewing, not tracking: started elsewhere
        observe_slewing(*driver, *mount, clock);
        mount->clear_log();
        mount->set_status("nNpEW180#");
        REQUIRE(wait_slew_done(*driver, clock));
        CHECK(mount->count(":ST6") == 0);
    }

    SECTION("tracking lost during the GOTO: restored once") {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0));
        mount->set_status("pEW180#");  // slewing, tracking
        observe_slewing(*driver, *mount, clock);
        mount->clear_log();
        mount->set_status("nNpEW180#");  // done, tracking off
        REQUIRE(wait_slew_done(*driver, clock));
        CHECK(mount->count(":ST6") == 1);
    }

    driver->set_connected(false);
}

namespace {

// A fake mount that reports a firmware version and a :GXEE# reply (#1216).
struct RefractionFake {
    std::string version;  // :GVN# reply, without '#'
    std::string gxee;     // :GXEE# reply (bare digit; empty = no reply)
    std::atomic<int> gxee_queries{0};

    std::string reply(const std::string& chunk) {
        if (chunk.rfind(":GVP", 0) == 0) return "On-Step#";
        if (chunk.rfind(":GVN", 0) == 0) return version + "#";
        if (chunk.rfind(":GXEE", 0) == 0) {
            ++gxee_queries;
            return gxee;
        }
        if (chunk.rfind(":GU", 0) == 0) return "nNpEo180#";
        if (chunk.rfind(":S", 0) == 0) return "1";
        return "0#";
    }
};

}  // namespace

// Decision 0016: get_device_firmware() takes only firmware_mutex_, which the
// connect held across its :GVP#/:GVN# round trips, so a hook called during
// the handshake waited for the serial reads (the contract sweep's hook bound
// failed on a loaded CI runner). The fake holds its :GVP# reply until
// released; the hook must return before that.
TEST_CASE("OnStep Telescope Driver - the firmware hook does not wait for the connect's version reads",
          "[onstep][telescope][unit]") {
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool released = false;
    std::atomic<bool> gvp_asked{false};
    alpacacore::test::FakeMountServer server([&](const std::string& chunk) -> std::string {
        if (chunk.rfind(":GVP", 0) == 0) {
            gvp_asked = true;
            std::unique_lock<std::mutex> lock(gate_mutex);
            gate_cv.wait(lock, [&] { return released; });
            return "On-Step#";
        }
        if (chunk.rfind(":GVN", 0) == 0) return "10.23a#";
        if (chunk.rfind(":GU", 0) == 0) return "nNpEo180#";
        if (chunk.rfind(":S", 0) == 0) return "1";
        return "0#";
    });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 10000;  // the held reply must not time out first
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);
    const auto release = [&] {
        std::lock_guard<std::mutex> lock(gate_mutex);
        released = true;
        gate_cv.notify_all();
    };

    std::thread connector([&] { (void)alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(20)); });
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!gvp_asked && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gvp_asked);
    // Releases the reply after 1.2 s, past the 1 s bound below, so a hook that
    // waits still returns and still fails the bound.
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        release();
    });
    const auto t0 = std::chrono::steady_clock::now();
    (void)driver->get_device_firmware();
    const auto took = std::chrono::steady_clock::now() - t0;
    release();
    releaser.join();
    connector.join();
    CHECK(took < std::chrono::seconds(1));
    CHECK(driver->get_device_firmware().value_or("") == "On-Step 10.23a");
    driver->set_connected(false);
}

TEST_CASE("OnStep Telescope Driver - DoesRefraction follows the OnStepX coordinate mode", "[onstep][telescope][unit]") {
    auto fake = std::make_shared<RefractionFake>();
    std::string version = "10.23a";
    std::string gxee = "1";
    bool expect_query = true;
    bool from_firmware = true;
    bool expected = true;

    SECTION("topocentric firmware (:GXEE# 1) applies refraction") {}
    SECTION("topocentric strict firmware (:GXEE# 2) applies refraction") { gxee = "2"; }
    SECTION("observed-place firmware (:GXEE# 0) does not") {
        gxee = "0";
        expected = false;
    }
    SECTION("OnStepX that does not answer :GXEE# keeps the local flag") {
        gxee = "";
        from_firmware = false;
        expected = false;
    }
    SECTION("legacy OnStep is not asked, since it answers unknown commands with 0") {
        version = "4.24s";
        gxee = "0";
        expect_query = false;
        from_firmware = false;
        expected = false;
    }
    fake->version = version;
    fake->gxee = gxee;

    alpacacore::test::FakeMountServer server([fake](const std::string& chunk) { return fake->reply(chunk); });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 200;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);

    require_alpaca_error([&] { (void)driver->get_does_refraction(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_does_refraction(true); }, alpacacore::AlpacaError::NotConnected);

    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    CHECK((fake->gxee_queries.load() > 0) == expect_query);
    CHECK(driver->get_does_refraction() == expected);

    if (from_firmware) {
        REQUIRE_NOTHROW(driver->set_does_refraction(expected));  // the firmware's own value
        require_alpaca_error([&] { driver->set_does_refraction(!expected); },
                             alpacacore::AlpacaError::PropertyNotImplemented);
        CHECK(driver->get_does_refraction() == expected);
    } else {
        REQUIRE_NOTHROW(driver->set_does_refraction(true));
        CHECK(driver->get_does_refraction());
        REQUIRE_NOTHROW(driver->set_does_refraction(false));
        CHECK_FALSE(driver->get_does_refraction());
    }

    driver->set_connected(false);
    require_alpaca_error([&] { (void)driver->get_does_refraction(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("OnStep Telescope Driver - the tracking restore never outlives its slew", "[onstep][telescope][unit]") {
    alpacacore::test::FakeTaskClock clock;  // every driver timer (decision 0005); outlives the driver
    auto mount = std::make_shared<ScriptedOnStep>();
    mount->set_status("NpEW180#");  // tracking, not slewing
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 200;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    REQUIRE(driver->get_tracking());

    // A slew the driver did not start (handset, another program) that ends
    // with tracking off: a left-over flag would restart tracking here.
    const auto foreign_slew_ends_untracked = [&]() {
        mount->set_status("npEW180#");  // slewing, not tracking
        observe_slewing(*driver, *mount, clock);
        mount->set_status("nNpEW180#");
        REQUIRE(wait_slew_done(*driver, clock));
    };

    SECTION("synchronous GOTO, then a foreign slew: no restore") {
        REQUIRE_NOTHROW(
            run_synchronous_slew(clock, [&] { driver->slew_to_coordinates(12.0, 45.0); }));  // ends after the 2 s grace
        mount->clear_log();
        foreign_slew_ends_untracked();
        CHECK(mount->count(":ST6") == 0);
    }

    SECTION("synchronous GOTO that loses tracking: restored once at its end") {
        REQUIRE_NOTHROW(run_synchronous_slew(
            clock, [&] { driver->slew_to_coordinates(12.0, 45.0); },
            [&] {
                mount->clear_log();
                mount->set_status("nNpEW180#");  // done, tracking off
            }));
        CHECK(mount->count(":ST6") == 1);
    }

    SECTION("Tracking=false during a GOTO wins over the restore") {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0));
        mount->set_status("pEW180#");  // slewing, tracking
        observe_slewing(*driver, *mount, clock);
        REQUIRE_NOTHROW(driver->set_tracking(false));
        mount->clear_log();
        mount->set_status("nNpEW180#");  // done, tracking off as asked
        REQUIRE(wait_slew_done(*driver, clock));
        CHECK(mount->count(":ST6") == 0);
    }

    SECTION("AbortSlew with tracking kept, then a foreign slew: no restore") {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0));
        mount->set_status("NpEW180#");  // stopped, still tracking
        REQUIRE_NOTHROW(driver->abort_slew());
        CHECK(mount->count(":ST6") == 0);
        mount->clear_log();
        foreign_slew_ends_untracked();
        CHECK(mount->count(":ST6") == 0);
    }

    SECTION("AbortSlew of a GOTO that lost tracking: restored at once, not later") {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0));
        mount->set_status("nNpEW180#");  // stopped, tracking off
        mount->clear_log();
        REQUIRE_NOTHROW(driver->abort_slew());
        CHECK(mount->count(":ST6") == 1);
        mount->clear_log();
        foreign_slew_ends_untracked();
        CHECK(mount->count(":ST6") == 0);
    }

    driver->set_connected(false);
}

namespace {

// The error code and text an AlpacaException carries, or -1 when `fn` returns.
std::pair<int, std::string> alpaca_error_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& ex) {
        return {ex.error_code(), ex.what()};
    }
    return {-1, ""};
}

}  // namespace

// #1274: ITelescopeV4 refuses an equatorial slew while Tracking is false.
// OnStepX turns tracking on at a GOTO only in some cases (the first GOTO after a
// home reset, or one that starts from home), so this is the interface contract.
TEST_CASE("OnStep Telescope Driver - an equatorial slew needs Tracking", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ScriptedOnStep>();
    mount->set_status("nNpEW180#");  // not tracking, not slewing, not parked
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 200;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    REQUIRE_FALSE(driver->get_tracking());
    driver->set_target_right_ascension(12.0);
    driver->set_target_declination(45.0);
    mount->clear_log();

    const auto nothing_sent = [&]() {
        CHECK(mount->count(":Sr") == 0);
        CHECK(mount->count(":Sd") == 0);
        CHECK(mount->count(":MS#") == 0);
        CHECK_FALSE(driver->get_slewing());
    };

    SECTION("all four forms are InvalidOperation and nothing reaches the mount") {
        require_alpaca_error([&]() { driver->slew_to_coordinates_async(12.0, 45.0); },
                             alpacacore::AlpacaError::InvalidOperation);
        require_alpaca_error([&]() { driver->slew_to_coordinates(12.0, 45.0); },
                             alpacacore::AlpacaError::InvalidOperation);
        // Each form refuses under its own name (#1379).
        for (const auto& [form, who] : std::vector<std::pair<std::function<void()>, std::string>>{
                 {[&]() { driver->slew_to_target_async(); }, "SlewToTargetAsync"},
                 {[&]() { driver->slew_to_target(); }, "SlewToTarget"}}) {
            const auto error = alpaca_error_of(form);
            CHECK(error.first == alpacacore::AlpacaError::InvalidOperation);
            CHECK(error.second == who + " requires Tracking to be true");
        }
        nothing_sent();
    }

    SECTION("a bad coordinate is InvalidValue first") {
        require_alpaca_error([&]() { driver->slew_to_coordinates_async(25.0, 45.0); },
                             alpacacore::AlpacaError::InvalidValue);
        nothing_sent();
    }

    SECTION("parked is InvalidWhileParked first") {
        mount->set_status("nNPHEo180#");  // parked at home, not tracking
        driver->set_connected(false);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        mount->clear_log();
        require_alpaca_error([&]() { driver->slew_to_coordinates_async(12.0, 45.0); },
                             alpacacore::AlpacaError::InvalidWhileParked);
        nothing_sent();
    }

    SECTION("a Tracking=false just written counts as off") {
        mount->set_status("NpEW180#");  // the mount still reports tracking
        REQUIRE_NOTHROW(driver->set_tracking(true));
        REQUIRE_NOTHROW(driver->set_tracking(false));
        mount->clear_log();
        require_alpaca_error([&]() { driver->slew_to_coordinates_async(12.0, 45.0); },
                             alpacacore::AlpacaError::InvalidOperation);
        nothing_sent();
    }

    SECTION("with Tracking on the slew goes out") {
        REQUIRE_NOTHROW(driver->set_tracking(true));
        mount->set_status("NpEW180#");
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0));
        CHECK(mount->count(":MS#") == 1);
    }

    driver->set_connected(false);
}

// #1377: ITelescopeV4 refuses an Alt/Az slew while Tracking is true (a tracking
// mount drifts off the requested Alt/Az). Without Tracking it goes out.
TEST_CASE("OnStep Telescope Driver - an Alt/Az slew is refused while Tracking is true", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ScriptedOnStep>();
    mount->set_status("nNpEW180#");  // not tracking, not slewing, not parked
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 200;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    REQUIRE_FALSE(driver->get_tracking());
    mount->clear_log();

    const auto nothing_sent = [&]() {
        CHECK(mount->count(":Sr") == 0);
        CHECK(mount->count(":Sd") == 0);
        CHECK(mount->count(":MS#") == 0);
    };

    SECTION("with Tracking off the slew goes out") {
        REQUIRE_NOTHROW(driver->slew_to_alt_az_async(45.0, 180.0));
        CHECK(mount->count(":MS#") == 1);
    }

    SECTION("with Tracking on both forms are InvalidOperation under their own names") {
        REQUIRE_NOTHROW(driver->set_tracking(true));
        mount->set_status("NpEW180#");  // tracking
        mount->clear_log();
        const auto async_error = alpaca_error_of([&]() { driver->slew_to_alt_az_async(45.0, 180.0); });
        CHECK(async_error.first == alpacacore::AlpacaError::InvalidOperation);
        CHECK(async_error.second.rfind("SlewToAltAzAsync ", 0) == 0);
        const auto sync_error = alpaca_error_of([&]() { driver->slew_to_alt_az(45.0, 180.0); });
        CHECK(sync_error.first == alpacacore::AlpacaError::InvalidOperation);
        CHECK(sync_error.second.rfind("SlewToAltAz ", 0) == 0);
        nothing_sent();
    }

    SECTION("a bad altitude is InvalidValue first") {
        REQUIRE_NOTHROW(driver->set_tracking(true));
        mount->clear_log();
        require_alpaca_error([&]() { driver->slew_to_alt_az_async(91.0, 180.0); },
                             alpacacore::AlpacaError::InvalidValue);
        nothing_sent();
    }

    SECTION("parked is InvalidWhileParked first, under the Alt/Az name") {
        mount->set_status("NPHEo180#");  // parked at home, tracking flag set
        driver->set_connected(false);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        mount->clear_log();
        const auto error = alpaca_error_of([&]() { driver->slew_to_alt_az_async(45.0, 180.0); });
        CHECK(error.first == alpacacore::AlpacaError::InvalidWhileParked);
        CHECK(error.second.rfind("SlewToAltAzAsync ", 0) == 0);
        nothing_sent();
    }

    driver->set_connected(false);
}

// #1379: OnStepX answers a GOTO sent while one runs by stopping the running one
// (Goto.cpp request(): CE_SLEW_IN_SLEW -> abort(), :MS# reply 5). The driver
// refuses the new slew first, so the running GOTO, park or home keeps going.
TEST_CASE("OnStep Telescope Driver - a slew while a GOTO runs is refused and the GOTO keeps going",
          "[onstep][telescope][unit]") {
    alpacacore::test::FakeTaskClock clock;  // the slew grace and the 2 s cache (decision 0005); outlives the driver
    auto mount = std::make_shared<ScriptedOnStep>();
    mount->set_status("NpEW180#");  // tracking, not slewing
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 200;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    REQUIRE(driver->get_tracking());
    driver->set_target_right_ascension(12.0);
    driver->set_target_declination(45.0);

    // A synchronous slew that is wrongly sent waits for the GOTO on the fake clock,
    // so each call runs on a thread with a real-time bound: a regression fails
    // here instead of hanging (the thread is released by ending the GOTO).
    const auto call_bounded = [&](const std::function<void()>& slew) {
        std::pair<int, std::string> error{-1, ""};
        std::atomic<bool> done{false};
        std::thread caller([&] {
            error = alpaca_error_of(slew);
            done = true;
        });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!done && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const bool returned = done;
        if (!returned) {
            mount->set_status("NpEW180#");  // end the GOTO so the waiting slew returns
            for (int i = 0; i < 200 && !done; ++i) {
                clock.advance(std::chrono::milliseconds(250));
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        caller.join();
        CHECK(returned);  // a refused slew returns at once
        return error;
    };
    const auto refused_and_left_running = [&](const std::function<void()>& slew, const char* who) {
        mount->clear_log();
        const auto error = call_bounded(slew);
        INFO(error.second);
        CHECK(error.first == alpacacore::AlpacaError::InvalidOperation);
        CHECK(error.second == std::string(who) + " is not allowed while a GOTO is running");
        CHECK(mount->count(":Sr") == 0);
        CHECK(mount->count(":MS#") == 0);
        CHECK(mount->count(":Q") == 0);  // nothing is stopped
    };

    SECTION("our own GOTO, before the mount shows it") {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0));
        refused_and_left_running([&]() { driver->slew_to_coordinates_async(13.0, 40.0); }, "SlewToCoordinatesAsync");
        CHECK(driver->get_slewing());
    }

    SECTION("our own GOTO the mount shows: every slew form") {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0));
        mount->set_status("pEW180#");  // slewing, tracking
        observe_slewing(*driver, *mount, clock);
        refused_and_left_running([&]() { driver->slew_to_coordinates_async(13.0, 40.0); }, "SlewToCoordinatesAsync");
        refused_and_left_running([&]() { driver->slew_to_target_async(); }, "SlewToTargetAsync");
        refused_and_left_running([&]() { driver->slew_to_coordinates(13.0, 40.0); }, "SlewToCoordinates");
        refused_and_left_running([&]() { driver->slew_to_target(); }, "SlewToTarget");
        CHECK(driver->get_slewing());
    }

    SECTION("our own GOTO the mount shows, with Tracking off: the Alt/Az forms") {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0));
        mount->set_status("npEW180#");  // slewing, not tracking: the Alt/Az check passes
        observe_slewing(*driver, *mount, clock);
        refused_and_left_running([&]() { driver->slew_to_alt_az_async(45.0, 180.0); }, "SlewToAltAzAsync");
        refused_and_left_running([&]() { driver->slew_to_alt_az(45.0, 180.0); }, "SlewToAltAz");
        CHECK(driver->get_slewing());
    }

    SECTION("a GOTO started elsewhere (handset)") {
        mount->set_status("pEW180#");                    // slewing, tracking: not ours
        clock.advance(std::chrono::milliseconds(2100));  // past the status cache
        refused_and_left_running([&]() { driver->slew_to_coordinates_async(13.0, 40.0); }, "SlewToCoordinatesAsync");
    }

    SECTION("a park already moving") {
        REQUIRE_NOTHROW(driver->park());
        mount->set_status("pIEW180#");  // parking: a GOTO to the park position
        clock.advance(std::chrono::milliseconds(2100));
        refused_and_left_running([&]() { driver->slew_to_coordinates_async(13.0, 40.0); }, "SlewToCoordinatesAsync");
        CHECK(driver->get_slewing());
    }

    SECTION("once the GOTO has ended the next slew goes out") {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0));
        mount->set_status("pEW180#");
        observe_slewing(*driver, *mount, clock);
        mount->set_status("NpEW180#");  // arrived, tracking
        REQUIRE(wait_slew_done(*driver, clock));
        mount->clear_log();
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(13.0, 40.0));
        CHECK(mount->count(":MS#") == 1);
    }

    SECTION("after an AbortSlew the next slew goes out, inside what was the slew grace") {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 45.0));
        mount->set_status("NpEW180#");          // stopped
        REQUIRE_NOTHROW(driver->abort_slew());  // ends the 2 s grace; no clock advance
        mount->clear_log();
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(13.0, 40.0));
        CHECK(mount->count(":MS#") == 1);
    }

    driver->set_connected(false);
}

// AGENTS.md: InvalidValue precedes the connection check. The slews used to check
// the connection (and the park state) before their coordinates.
TEST_CASE("OnStep Telescope Driver - slews and syncs check their coordinates before the connection",
          "[onstep][telescope][unit]") {
    auto driver = make_driver(0);  // never connected
    const double nan = std::numeric_limits<double>::quiet_NaN();
    require_alpaca_error([&]() { driver->slew_to_coordinates_async(24.0, 0.0); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->slew_to_coordinates(12.0, 90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->slew_to_alt_az_async(91.0, 0.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->slew_to_alt_az(45.0, nan); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->slew_to_coordinates_async(12.0, 45.0); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->slew_to_alt_az_async(45.0, 180.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->sync_to_coordinates(24.0, 0.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->sync_to_coordinates(12.0, 45.0); }, alpacacore::AlpacaError::NotConnected);
}

// #1379: every :MS# refusal used to read "below horizon or limit". The error now
// names the reason from the reply digit (OnStepX Goto.command.cpp).
TEST_CASE("OnStep Telescope Driver - a refused GOTO reports the mount's reason", "[onstep][telescope][unit]") {
    auto mount = std::make_shared<ScriptedOnStep>();
    mount->set_status("NpEW180#");  // tracking, not slewing
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 200;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    struct Case {
        const char* reply;
        int code;
        const char* reason;
    };
    SECTION("each OnStepX refusal digit") {
        for (const Case& c : {Case{"1", alpacacore::AlpacaError::InvalidOperation, "below the horizon limit"},
                              Case{"2", alpacacore::AlpacaError::InvalidOperation, "above the overhead limit"},
                              Case{"3", alpacacore::AlpacaError::InvalidOperation, "in standby"},
                              Case{"4", alpacacore::AlpacaError::InvalidWhileParked, "the mount is parked"},
                              Case{"5", alpacacore::AlpacaError::InvalidOperation, "the mount stopped it"},
                              Case{"6", alpacacore::AlpacaError::InvalidOperation, "outside the limits"},
                              Case{"7", alpacacore::AlpacaError::InvalidOperation, "a hardware fault"},
                              Case{"8", alpacacore::AlpacaError::InvalidOperation, "already moving"},
                              Case{"9", alpacacore::AlpacaError::InvalidOperation, "an unspecified error"}}) {
            mount->set_ms_reply(c.reply);
            const auto error = alpaca_error_of([&]() { driver->slew_to_coordinates_async(12.0, 45.0); });
            INFO("reply " << c.reply << ": " << error.second);
            CHECK(error.first == c.code);
            CHECK(error.second.rfind("SlewToCoordinatesAsync: the mount refused the GOTO: ", 0) == 0);
            CHECK(error.second.find(c.reason) != std::string::npos);
            CHECK(error.second.find(std::string("reply '") + c.reply + "'") != std::string::npos);
            CHECK_FALSE(driver->get_slewing());  // a refused GOTO is not a slew
        }
    }

    // OnStep and OnStepX reply with one digit (Command.ino, Goto.command.cpp); a
    // reply outside 0-9 still refuses, and the message shows what came back.
    SECTION("a reply that is not a known digit is an unknown reason") {
        mount->set_ms_reply("X");
        const auto error = alpaca_error_of([&]() { driver->slew_to_coordinates_async(12.0, 45.0); });
        INFO(error.second);
        CHECK(error.first == alpacacore::AlpacaError::InvalidOperation);
        CHECK(error.second.find("an unknown reason") != std::string::npos);
        CHECK(error.second.find("reply 'X'") != std::string::npos);
    }

    driver->set_connected(false);
}

// #1379: Tracking, SetPark, Unpark and Sync sent their commands blind or ignored
// the reply, so a mount that refused them read as success. Each now reads the
// reply (a bare '0'/'1', or N/A# / En# for :CM#) and throws on a refusal, with
// the :GE# reason; a '0' that :GE# then calls no error is taken (as for :hP#).
TEST_CASE("OnStep Telescope Driver - a Tracking write SetPark Unpark or Sync the mount refuses throws",
          "[onstep][telescope][unit]") {
    alpacacore::test::FakeTaskClock clock;  // the 2 s status cache (decision 0005); outlives the driver
    auto mount = std::make_shared<ScriptedOnStep>();
    mount->set_status("NpEW180#");  // tracking, not slewing, not parked
    alpacacore::test::FakeMountServer server([mount](const std::string& chunk) { return mount->reply(chunk); });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 200;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    const auto refused = [&](const std::function<void()>& call, const std::string& prefix, const char* reason) {
        const auto error = alpaca_error_of(call);
        INFO(error.second);
        CHECK(error.first == alpacacore::AlpacaError::InvalidOperation);
        CHECK(error.second.rfind(prefix, 0) == 0);
        CHECK(error.second.find(reason) != std::string::npos);
    };

    SECTION("a Tracking write the mount refuses throws and leaves Tracking as the mount has it") {
        mount->refuse(":ST", "22#");  // OnStep 4.x: in motion
        refused([&]() { driver->set_tracking(false); }, "Tracking refused by the mount", "mount still in motion");
        CHECK(driver->get_tracking());  // the cache was not patched to the refused value
        CHECK(mount->count(":ST0#") == 1);
    }

    SECTION("a refused SetPark throws") {
        mount->refuse(":hQ", "22#");  // OnStepX Park::set(): a GOTO or guide move runs
        refused([&]() { driver->set_park(); }, "SetPark refused by the mount", "mount still in motion");
    }

    SECTION("a '0' that :GE# calls no error is taken") {
        mount->refuse(":hQ", "00#");
        REQUIRE_NOTHROW(driver->set_park());
        mount->refuse(":ST", "25#");
        REQUIRE_NOTHROW(driver->set_tracking(false));
    }

    // #1379 review: firmware without :GE# answers it with no two-digit code. The
    // driver has then just re-read :GU#, which decides where it can.
    SECTION("a 0 with no :GE# code: :GU# decides for Unpark and Tracking, SetPark stays refused") {
        mount->refuse(":hR", "?#");         // not a code
        REQUIRE_NOTHROW(driver->unpark());  // :GU# shows not parked: taken
        CHECK_FALSE(driver->get_at_park());
        mount->refuse(":ST", "?#");
        REQUIRE_NOTHROW(driver->set_tracking(true));  // :GU# already shows tracking: taken
        refused([&]() { driver->set_tracking(false); }, "Tracking refused by the mount", "no reason given");
        mount->refuse(":hQ", "?#");
        refused([&]() { driver->set_park(); }, "SetPark refused by the mount", "no reason given");
        mount->set_status("nNPHEo180#");  // parked
        driver->set_connected(false);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        mount->refuse(":hR", "?#");
        refused([&]() { driver->unpark(); }, "Unpark refused by the mount", "no reason given");
        CHECK(driver->get_at_park());
    }

    SECTION("a refused Unpark throws and the mount stays parked; 'not parked' is a no-op") {
        mount->set_status("nNPHEo180#");  // parked at home
        driver->set_connected(false);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        REQUIRE(driver->get_at_park());
        mount->refuse(":hR", "21#");  // OnStepX Park::restore(): motor fault
        refused([&]() { driver->unpark(); }, "Unpark refused by the mount", "hardware fault");
        CHECK(driver->get_at_park());
        mount->refuse(":hR", "11#");  // not parked
        REQUIRE_NOTHROW(driver->unpark());
        CHECK_FALSE(driver->get_at_park());
    }

    SECTION("a Sync the mount refuses throws with the reason; both forms under their names") {
        mount->set_cm_reply("E6#");  // outside limits
        auto error = alpaca_error_of([&]() { driver->sync_to_coordinates(12.0, 45.0); });
        INFO(error.second);
        CHECK(error.first == alpacacore::AlpacaError::InvalidOperation);
        CHECK(error.second.rfind("SyncToCoordinates: the mount refused the sync: ", 0) == 0);
        CHECK(error.second.find("outside the limits") != std::string::npos);
        CHECK(error.second.find(":CM# reply 'E6'") != std::string::npos);
        driver->set_target_right_ascension(12.0);
        driver->set_target_declination(45.0);
        error = alpaca_error_of([&]() { driver->sync_to_target(); });
        CHECK(error.second.rfind("SyncToTarget: ", 0) == 0);
        mount->set_cm_reply("N/A#");
        REQUIRE_NOTHROW(driver->sync_to_coordinates(12.0, 45.0));
    }

    SECTION("a Sync during a GOTO is refused before anything is sent") {
        mount->set_status("pEW180#");                    // a GOTO runs
        clock.advance(std::chrono::milliseconds(2100));  // past the status cache
        mount->clear_log();
        const auto error = alpaca_error_of([&]() { driver->sync_to_coordinates(12.0, 45.0); });
        CHECK(error.first == alpacacore::AlpacaError::InvalidOperation);
        CHECK(error.second == "SyncToCoordinates is not allowed while a GOTO is running");
        CHECK(mount->count(":Sr") == 0);
        CHECK(mount->count(":CM#") == 0);
    }

    driver->set_connected(false);
}

// #910: set_connected(true) holds the driver mutex for the whole connect, so a
// Connected read that took it waited for the whole connect. The router checks
// Connecting before its own Connected reads; DeviceState does not, and a read
// that lands just before the connect task shows as running gets past it.
TEST_CASE("OnStep Telescope Driver - Connected answers while a connect is in flight", "[onstep][telescope][unit]") {
    std::atomic<bool> in_slow_reply{false};
    std::atomic<bool> release{false};
    alpacacore::test::FakeMountServer server([&](const std::string& chunk) {
        if (chunk.rfind(":GVN", 0) == 0) {
            // The firmware query runs inside set_connected(true), under mutex_.
            in_slow_reply = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!release && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return std::string("10.23a#");
        }
        if (chunk.size() >= 2 && chunk[0] == 'K') {
            return std::string(1, chunk[1]) + "#";
        }
        return std::string("0#");
    });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 5000;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);

    driver->connect();
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!in_slow_reply && std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(in_slow_reply);  // the connect is now parked in the firmware query
    REQUIRE(driver->get_connecting());

    auto start = std::chrono::steady_clock::now();
    const bool connected_mid_connect = driver->get_connected();
    const auto connected_elapsed = std::chrono::steady_clock::now() - start;
    // DeviceState has no Connecting gate in the router: it is the read that stalled.
    start = std::chrono::steady_clock::now();
    const auto state_mid_connect = driver->get_device_state();
    const auto state_elapsed = std::chrono::steady_clock::now() - start;
    release = true;
    CHECK(connected_elapsed < std::chrono::milliseconds(500));
    CHECK_FALSE(connected_mid_connect);  // Connected stays false until the connect has finished
    CHECK(state_elapsed < std::chrono::milliseconds(500));
    CHECK(state_mid_connect.empty());

    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(driver->get_connected());
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());

    // A reconnect starts from a cleared flag: the first session's published
    // Connected must not show through the second connect's warm-up.
    in_slow_reply = false;
    release = false;
    driver->connect();
    const auto again_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!in_slow_reply && std::chrono::steady_clock::now() < again_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool held_again = in_slow_reply;
    const bool connected_mid_reconnect = driver->get_connected();
    release = true;
    REQUIRE(held_again);
    CHECK_FALSE(connected_mid_reconnect);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

#endif  // _WIN32

#ifndef _WIN32
// #1298: the LST helper cut UTC to whole seconds.
TEST_CASE("OnStep Telescope Driver - SiderealTime follows the clock to under a second", "[onstep][telescope][unit]") {
    alpacacore::test::FakeMountServer server([](const std::string& chunk) {
        if (chunk.size() >= 2 && chunk[0] == 'K') {
            return std::string(1, chunk[1]) + "#";
        }
        return std::string("0#");
    });
    REQUIRE(server.ok());
    alpacacore::vendor::onstep::ConnectionInfo conn;
    conn.type = alpacacore::vendor::onstep::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 50;
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    alpacacore::test::check_sidereal_time_follows_the_clock(*driver);
    driver->set_connected(false);
}
#endif

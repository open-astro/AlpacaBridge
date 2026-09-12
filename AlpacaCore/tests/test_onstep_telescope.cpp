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

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include "catch2_compat.h"
#ifndef _WIN32
#include "concurrency_stress.h"
#include "fake_mount_server.h"
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
    CHECK(driver->get_can_move_axis(0));
    CHECK(driver->get_can_move_axis(1));
    CHECK_FALSE(driver->get_can_move_axis(2));
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

TEST_CASE("OnStep Telescope Driver - a slow mount ack does not turn an accurate client clock into a warning",
          "[onstep][telescope][unit]") {
    // The offset is sampled BEFORE the mount write (review round 2 on #471):
    // set_time() is three serial round trips (:SL, :SC, :SG) against a 2 s
    // threshold, so a mount that takes 750 ms to ack each one (2.25 s in
    // all, each under the wrapper's 1 s per-ack timeout) would otherwise
    // make a perfectly set client clock read as seconds out. With the offset
    // sampled after the write, this case logs a line; sampled before, nothing.
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

    alpacacore::test::FakeMountServer server([](const std::string& chunk) {
        if (chunk.rfind(":SL", 0) == 0 || chunk.rfind(":SC", 0) == 0 || chunk.rfind(":SG", 0) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(750));  // the slow ack, x3
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

    driver->set_utc_date(std::chrono::system_clock::now());  // accurate client, slow mount
    CHECK(warns.load() == 0);
    driver->set_connected(false);
}

#endif  // _WIN32

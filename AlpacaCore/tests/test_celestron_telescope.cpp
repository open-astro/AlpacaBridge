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
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/celestron/celestron_cgx_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "gps_status_check.h"
#include "sidereal_time_check.h"

#ifndef _WIN32
#include "concurrency_stress.h"
#include "fake_celestron_cgx_mount.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"
#endif

using alpacacore::DeviceType;

namespace {

using alpacacore::test::parse_timestamp;
using alpacacore::test::require_alpaca_error;
using alpacacore::test::time_stamp_of;
using alpacacore::test::timed;

} // namespace

TEST_CASE("Celestron Telescope Driver - Defaults", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_device_type() == DeviceType::Telescope);
    REQUIRE_FALSE(driver->get_connected());

    CHECK(driver->get_name() == "Celestron NexStar Telescope");

    REQUIRE(driver->get_can_slew());
    REQUIRE(driver->get_can_slew_async());
    REQUIRE_FALSE(driver->get_can_slew_alt_az());
    REQUIRE_FALSE(driver->get_can_slew_alt_az_async());
    REQUIRE(driver->get_can_sync());
    REQUIRE_FALSE(driver->get_can_sync_alt_az());
    REQUIRE(driver->get_can_find_home());
    REQUIRE(driver->get_can_park());
    REQUIRE(driver->get_can_unpark());
    REQUIRE(driver->get_can_set_park());
    REQUIRE_FALSE(driver->get_can_pulse_guide());
    REQUIRE_FALSE(driver->get_can_set_guide_rates());
    REQUIRE(driver->get_can_set_tracking());
}

TEST_CASE("Celestron Telescope Driver - Device metadata", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(3, conn);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Celestron NexStar Mount Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Celestron NexStar Driver v0.1");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "Celestron_3");
}

#ifndef _WIN32
TEST_CASE("Celestron serial backends - refuse a NexStar open of a CGX AUX port",
          "[celestron][telescope][protocol][unit]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::vendor::celestron::CelestronCgxProtocol direct;
    direct.connect_serial(mount.slave_path());

    auto& nexstar = alpacacore::vendor::celestron::CelestronProtocolWrapper::instance();
    nexstar.disconnect();
    alpacacore::vendor::celestron::ConnectionInfo info;
    info.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    info.port_path = mount.slave_path();
    // SerialLink (#1202, #1448) refuses the held port before opening it: the
    // connect fails, names the CGX link that holds it, and leaves it working.
    CHECK_FALSE(nexstar.connect(info));
    CHECK_FALSE(nexstar.is_connected());
    CHECK(nexstar.last_connect_error().find("Celestron CGX mount") != std::string::npos);
    CHECK(direct.is_connected());
}
#endif

TEST_CASE("Celestron Telescope Driver - Disconnected Behavior", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    REQUIRE_FALSE(driver->get_connected());

    // Position queries require connection
    CHECK_THROWS_AS(driver->get_right_ascension(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_declination(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_altitude(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_azimuth(), alpacacore::AlpacaException);

    // Tracking requires connection
    CHECK_THROWS_AS(driver->get_tracking(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_tracking(true), alpacacore::AlpacaException);

    // Slew requires target set (throws before connection check)
    CHECK_THROWS_AS(driver->slew_to_target_async(), alpacacore::AlpacaException);

    // Park requires connection
    CHECK_THROWS_AS(driver->park(), alpacacore::AlpacaException);

    // Action and command methods
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("Celestron Telescope Driver - Unsupported actions", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("Celestron Telescope Driver - Target Range Validation", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    // Target not yet set
    REQUIRE_THROWS(driver->get_target_right_ascension());
    REQUIRE_THROWS(driver->get_target_declination());

    // RA range: [0, 24)
    REQUIRE_THROWS(driver->set_target_right_ascension(-0.1));
    REQUIRE_THROWS(driver->set_target_right_ascension(24.0));
    REQUIRE_NOTHROW(driver->set_target_right_ascension(0.0));
    REQUIRE_NOTHROW(driver->set_target_right_ascension(12.0));
    REQUIRE_NOTHROW(driver->set_target_right_ascension(23.999));

    // Dec range: [-90, 90]
    REQUIRE_THROWS(driver->set_target_declination(-90.1));
    REQUIRE_THROWS(driver->set_target_declination(90.1));
    REQUIRE_NOTHROW(driver->set_target_declination(-90.0));
    REQUIRE_NOTHROW(driver->set_target_declination(0.0));
    REQUIRE_NOTHROW(driver->set_target_declination(90.0));
    REQUIRE_NOTHROW(driver->set_target_declination(45.0));
}

TEST_CASE("Celestron Telescope Driver - Axis Rate Ranges", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    // Primary axis (RA/Azimuth)
    auto primary = driver->get_axis_rate_range(0);
    REQUIRE(primary.first == 0.0);
    REQUIRE(primary.second > primary.first);

    // Secondary axis (Dec/Altitude)
    auto secondary = driver->get_axis_rate_range(1);
    REQUIRE(secondary.first == 0.0);
    REQUIRE(secondary.second > secondary.first);

    // Tertiary axis not supported
    auto tertiary_ranges = driver->get_axis_rate_ranges(2);
    REQUIRE(tertiary_ranges.empty());

    REQUIRE_THROWS(driver->get_axis_rate_range(2));

    // Out-of-range axis raises InvalidValue from AxisRates (#516).
    require_alpaca_error([&]() { (void)driver->get_axis_rate_ranges(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { (void)driver->get_axis_rate_ranges(3); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("Celestron Telescope Driver - Target Coordinate Persistence", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(12.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 12.0);

    REQUIRE_NOTHROW(driver->set_target_declination(45.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 45.0);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(6.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 6.0);
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 45.0);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(0.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 0.0);
    REQUIRE_NOTHROW(driver->set_target_right_ascension(23.999));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 23.999);

    REQUIRE_NOTHROW(driver->set_target_declination(-90.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), -90.0);
    REQUIRE_NOTHROW(driver->set_target_declination(90.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 90.0);
}

TEST_CASE("Celestron Telescope Driver - Site Property Validation", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    require_alpaca_error([&]() { driver->set_site_elevation(-300.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_elevation(10000.1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_site_elevation(-300.0));
    REQUIRE_NOTHROW(driver->set_site_elevation(10000.0));
    ALPACA_REQUIRE_APPROX(driver->get_site_elevation(), 10000.0);

    require_alpaca_error([&]() { driver->set_site_latitude(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_latitude(90.1); }, alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&]() { driver->set_site_longitude(-180.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_longitude(180.1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("Celestron Telescope Driver - Telescope Properties", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    CHECK(driver->get_interface_version() >= 3);

    auto eq = driver->get_equatorial_system();
    CHECK((eq == alpacacore::EquatorialSystem::Topocentric ||
           eq == alpacacore::EquatorialSystem::J2000 ||
           eq == alpacacore::EquatorialSystem::Other));

    require_alpaca_error([&] { (void)driver->get_alignment_mode(); }, alpacacore::AlpacaError::NotConnected);

    auto rates = driver->get_tracking_rates();
    CHECK_FALSE(rates.empty());

    CHECK(driver->get_slew_settle_time() >= 0);
}

TEST_CASE("Celestron Telescope Driver - ASCOM Error Codes", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    require_alpaca_error([&]() { (void)driver->get_right_ascension(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_declination(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_altitude(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_azimuth(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_tracking(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_tracking(true); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->set_target_right_ascension(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_right_ascension(24.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(90.1); }, alpacacore::AlpacaError::InvalidValue);
}

// open-astro#769: MoveAxis validates its arguments before the connection check
// (driver-lifecycle.instructions.md error precedence), so a disconnected driver answers a bad
// argument with InvalidValue rather than NotConnected.
TEST_CASE("Celestron Telescope Driver - MoveAxis argument errors precede NotConnected",
          "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    require_alpaca_error([&]() { driver->move_axis(2, 0.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->move_axis(0, std::numeric_limits<double>::quiet_NaN()); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->move_axis(0, 1000.0); }, alpacacore::AlpacaError::InvalidValue);

    // Valid arguments still reach the connection check.
    require_alpaca_error([&]() { driver->move_axis(0, 0.0); }, alpacacore::AlpacaError::NotConnected);
}

// open-astro#346, the shape #304 fixed on the Sky-Watcher driver: ASCOM treats
// TargetRightAscension and TargetDeclination as independent properties, each
// throwing ValueNotSet until that property itself has been written. One shared
// flag let a write to either unlock both, so a client reading the target it
// did not set got a default 0 rather than an error -- which ConformU reports as
// "Read before write should generate an error and didn't".
TEST_CASE("Celestron Telescope Driver - the two target properties are independent", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

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

TEST_CASE("Celestron Telescope Driver - a far-off client UTCDate is logged once per connection on a disciplined host",
          "[celestron][telescope][unit]") {
    // Same contract as the OnStep case: the mount keeps its own clock and
    // UTCDate writes it, so the driver keeps aiming by the client's instant;
    // what it adds is the shared once-per-connection WARN on an
    // NTP-disciplined host. Without this case, deleting this driver's
    // warn_once() call left the suite green (review finding on #471).
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
            if (level == alpacacore::logging::LogLevel::Warn && component == "Celestron" &&
                message.find("Client UTCDate disagrees") != std::string::npos) {
                ++warns;
            }
        });

    // NexStar "H" (set time) ignores the reply; a position-pair sized reply
    // with the trailing '#' keeps every fixed-length connect read from
    // waiting out its timeout (see test_celestron_concurrency_stress.cpp).
    alpacacore::test::FakeMountServer server([](const std::string&) { return std::string("00000000,00000000#"); });
    REQUIRE(server.ok());
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    // 500 ms, not the stress files' 50 ms: the "H" reply is read one byte at
    // a time against the whole-response budget, and 50 ms is not enough for
    // the 18-byte canned reply even on loopback (timed out every run).
    conn.response_timeout_ms = 500;
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    const auto far = std::chrono::system_clock::now() + std::chrono::minutes(30);
    driver->set_utc_date(std::chrono::system_clock::now());  // agrees: no line, budget untouched
    CHECK(warns.load() == 0);
    driver->set_utc_date(far);
    CHECK(warns.load() == 1);
    driver->set_utc_date(far + std::chrono::seconds(1));
    CHECK(warns.load() == 1);

    // A reconnect re-arms it.
    driver->set_connected(false);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    driver->set_utc_date(far);
    CHECK(warns.load() == 2);
    driver->set_connected(false);
}

// ── PulseGuide on one axis leaves the other axis's pulse alone (#813) ────────
//
// MC_AUX_GUIDE takes at most 255 cs, so a longer pulse is continued by a
// chunk-chain thread. With one thread and one end time shared by both axes, a
// Dec pulse reaped the RA chain (the rest of a long RA pulse was never sent)
// and overwrote the RA end time (IsPulseGuiding went false while RA guided).

namespace {

// Records the MC_AUX_GUIDE duration byte (cs) of each pulse sent to one motor.
class AuxGuideLog {
public:
    void add(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back(command);
    }

    std::vector<int> guide_chunks_cs(char motor) {
        const std::string needle{'P', '\x03', motor, '\x26'};
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int> chunks;
        for (const auto& command : commands_) {
            for (auto pos = command.find(needle); pos != std::string::npos; pos = command.find(needle, pos + 1)) {
                if (pos + 5 < command.size()) {
                    chunks.push_back(static_cast<unsigned char>(command[pos + 5]));
                }
            }
        }
        return chunks;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> commands_;
};

// The status poll asks the mount whether it is slewing: the goto flag (L) on a hand controller, the slew-done
// query (MC_SLEW_DONE, 0x13) on the pass-through path this canned mount takes. The canned position pair is no
// answer to either, so these two get the "not slewing" replies.
std::optional<std::string> pulse_mount_idle_reply(const std::string& command) {
    if (!command.empty() && command[0] == 'L') return std::string("0#");
    if (command.size() > 3 && command[0] == 'P' && static_cast<unsigned char>(command[3]) == 0x13) {
        return std::string("\xFF#");
    }
    return std::nullopt;
}

std::unique_ptr<alpacacore::TelescopeDriver> connect_pulse_guide_mount(alpacacore::test::FakeMountServer& server) {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 500;  // see the UTCDate case above
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    // The canned reply answers the AUX-bus probe, so an autoguider port is found.
    REQUIRE(driver->get_can_pulse_guide());
    return driver;
}

}  // namespace

TEST_CASE("Celestron Telescope Driver - a Dec pulse does not cut the chunk chain of a long RA pulse",
          "[celestron][telescope][unit][pulseguide]") {
    auto log = std::make_shared<AuxGuideLog>();
    alpacacore::test::FakeMountServer server([log](const std::string& command) {
        log->add(command);
        if (const auto idle = pulse_mount_idle_reply(command)) return *idle;
        return std::string("00000000,00000000#");
    });
    REQUIRE(server.ok());
    auto driver = connect_pulse_guide_mount(server);

    const auto t0 = std::chrono::steady_clock::now();
    driver->pulse_guide(2, 5000);  // East, RA: 255 cs now, 245 cs chained at ~2.55 s
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    driver->pulse_guide(0, 3000);  // North, Dec: 255 cs now, 45 cs chained
    std::this_thread::sleep_until(t0 + std::chrono::milliseconds(4600));

    // The RA pulse runs until t0 + 5.0 s, plus the 1 s completion delay.
    CHECK(driver->get_is_pulse_guiding());
    CHECK(log->guide_chunks_cs('\x10') == std::vector<int>{255, 245});
    CHECK(log->guide_chunks_cs('\x11') == std::vector<int>{255, 45});
    driver->set_connected(false);
}

TEST_CASE("Celestron Telescope Driver - a short Dec pulse does not end IsPulseGuiding of a running RA pulse",
          "[celestron][telescope][unit][pulseguide]") {
    alpacacore::test::FakeMountServer server([](const std::string& command) {
        if (const auto idle = pulse_mount_idle_reply(command)) return *idle;
        return std::string("00000000,00000000#");
    });
    REQUIRE(server.ok());
    auto driver = connect_pulse_guide_mount(server);

    const auto t0 = std::chrono::steady_clock::now();
    driver->pulse_guide(2, 2000);  // East, RA, 2.0 s (one chunk)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    driver->pulse_guide(0, 200);  // North, Dec, 0.2 s: ends (with the delay) at ~1.3 s
    std::this_thread::sleep_until(t0 + std::chrono::milliseconds(1800));
    CHECK(driver->get_is_pulse_guiding());  // RA guides until t0 + 2.0 s + 1 s delay

    std::this_thread::sleep_until(t0 + std::chrono::milliseconds(3300));
    CHECK_FALSE(driver->get_is_pulse_guiding());  // both axes ended
    driver->set_connected(false);
}

// #627: `x < min || x > max` is false for NaN, so NaN passed every range check
// and was stored (targets, elevation) or reached the mount (the site latitude
// and longitude setters check the connection only AFTER the range, so a NaN
// used to surface as NotConnected instead of InvalidValue).
TEST_CASE("Celestron Telescope Driver - non-finite input is rejected", "[celestron][telescope][unit][nonfinite]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

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

// #627: Celestron's guide-rate setter only exists when the AUX bus probe found
// an autoguider port, so it is reachable only on a connected driver. The
// canned reply makes every bus probe answer, which the CanSetGuideRates
// precondition below confirms. NaN used to be converted to a percentage and
// written to the mount.
TEST_CASE("Celestron Telescope Driver - non-finite guide rate is rejected", "[celestron][telescope][unit][nonfinite]") {
    alpacacore::test::FakeMountServer server([](const std::string&) { return std::string("00000000,00000000#"); });
    REQUIRE(server.ok());
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 500;
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    REQUIRE(driver->get_can_set_guide_rates());

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    require_alpaca_error([&]() { driver->set_guide_rate({nan, 0.004}); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_guide_rate({0.004, nan}); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_guide_rate({inf, 0.004}); }, alpacacore::AlpacaError::InvalidValue);
    driver->set_connected(false);
}

// #627: `validate_ra_dec` is `ra < 0 || ra >= 24` / `dec < -90 || dec > 90`,
// both false for NaN, so a NaN coordinate went through slew and sync to the
// mount. Both check the connection before validating, so this needs a
// connected driver.
TEST_CASE("Celestron Telescope Driver - non-finite slew and sync coordinates are rejected",
          "[celestron][telescope][unit][nonfinite]") {
    alpacacore::test::FakeMountServer server([](const std::string&) { return std::string("00000000,00000000#"); });
    REQUIRE(server.ok());
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 500;
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);
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

    driver->set_connected(false);
}

// #781: AbortSlew ran the cancel and both axis stops bare, so a throw from the
// first skipped the other two, and a stop lost on a dead link was reported as a
// clean abort. Every stop is tried, a failure throws DriverException afterwards
// and Slewing is left alone; the success path is unchanged.
TEST_CASE("Celestron Telescope Driver - AbortSlew reports a stop it could not send", "[celestron][telescope][unit]") {
    auto log = std::make_shared<std::vector<std::string>>();
    auto log_mutex = std::make_shared<std::mutex>();
    alpacacore::test::FakeMountServer server([log, log_mutex](const std::string& command) {
        std::lock_guard<std::mutex> lock(*log_mutex);
        log->push_back(command);
        if (command.empty()) return std::string("#");
        switch (command[0]) {
            case 'V':
                return std::string("\x05\x23#", 3);
            case 'm':
                return std::string("\x18#", 2);
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return std::string("00000000,00000000#");
            case 'J':
                return std::string("\x01#", 2);
            case 'L':
                return std::string("0#");
            case 'M':
            case 'T':
                return std::string("#");
            case 'P':
                if (command.size() >= 4 && static_cast<unsigned char>(command[3]) == 0xFE) {
                    return std::string("\x01\x00#", 3);
                }
                return std::string("#");
            default:
                return std::string("#");
        }
    });
    REQUIRE(server.ok());
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 200;
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    driver->move_axis(0, 1.0);
    REQUIRE(driver->get_slewing());

    SECTION("success clears Slewing and sends the cancel and both axis stops") {
        {
            std::lock_guard<std::mutex> lock(*log_mutex);
            log->clear();
        }
        REQUIRE_NOTHROW(driver->abort_slew());
        CHECK_FALSE(driver->get_slewing());
        std::lock_guard<std::mutex> lock(*log_mutex);
        CHECK(std::count(log->begin(), log->end(), std::string("M")) == 1);
        const auto axis_stops = std::count_if(log->begin(), log->end(), [](const std::string& c) {
            return c.size() >= 5 && c[0] == 'P' && c[1] == '\x02' && (c[3] == '\x24' || c[3] == '\x25') && c[4] == '\0';
        });
        CHECK(axis_stops == 4);  // each axis gets both stop directions
    }

    SECTION("a dropped link throws DriverException and keeps the slew error observable") {
        REQUIRE(server.drop_connections());
        try {
            driver->abort_slew();
            FAIL("AbortSlew returned success although the stops could not be sent");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).rfind("AbortSlew stop failed: ", 0) == 0);
        }
        CHECK_FALSE(driver->get_at_home());
        try {
            (void)driver->get_slewing();
            FAIL("Slewing returned a completion value after the stop failed");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).rfind("AbortSlew stop failed: ", 0) == 0);
        }
    }

    driver->set_connected(false);
}

#endif  // _WIN32

// #1298: the LST helper cut UTC to whole seconds.
TEST_CASE("Celestron Telescope Driver - SiderealTime follows the clock to under a second",
          "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);
    alpacacore::test::check_sidereal_time_follows_the_clock(*driver);
}

#ifndef _WIN32

// Getters and DeviceState served from the polled snapshot (decision 0010, Celestron adoption; link-loss policy of
// decision 0009). The driver runs on a FakeTaskClock, so its status poll runs only when a case advances the clock,
// and the fake mount answers on the wall clock after an injected latency per transaction: a getter that asked the
// mount would pay it and show in the transaction count.
namespace {

using alpacacore::test::FakeTaskClock;
namespace cel = alpacacore::vendor::celestron;

constexpr auto kSnapshotLatency = std::chrono::milliseconds(100);
// A getter that reached the mount would pay kSnapshotLatency, so the budget only has to sit below it. Sanitized
// builds run several times slower on a loaded runner and get more room; the transaction count is the strict check.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ALPACACORE_CELESTRON_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define ALPACACORE_CELESTRON_SANITIZED 1
#endif
#endif
#if defined(ALPACACORE_CELESTRON_SANITIZED)
constexpr auto kSnapshotGetterBudget = std::chrono::milliseconds(60);
#else
constexpr auto kSnapshotGetterBudget = std::chrono::milliseconds(20);
#endif

struct SnapshotMount {
    std::atomic<int> transactions{0};
    std::atomic<int> latency_ms{0};
    std::atomic<bool> mute{false};
    std::atomic<bool> slewing{false};
    std::atomic<bool> tracking{true};
    std::atomic<char> pier{'W'};
    std::atomic<bool> shifted{false};
    std::atomic<bool> w_fail{false};  // the location query goes unanswered
    std::atomic<int> w_delay_ms{0};   // the location query answers after this long
    std::atomic<int> w_count{0};      // location queries received
    std::atomic<bool> l_fail{false};  // the slewing query goes unanswered
    std::atomic<bool> z_fail{false};  // the alt-az query goes unanswered
    std::atomic<int> slow_count{0};   // pier-side and tracking-mode queries received
    std::atomic<bool> level_mute{false};  // the FindHome level-done query goes unanswered
    std::atomic<int> level_count{0};      // level-done queries received
    std::atomic<int> m_delay_ms{0};       // the goto cancel (a stop exchange) answers after this long
    std::atomic<int> m_count{0};          // goto cancels received
    std::atomic<int> t_delay_ms{0};       // the tracking-mode write answers after this long
    std::atomic<int> t_count{0};          // tracking-mode writes received
    std::atomic<bool> h_mute{false};      // the time write goes unanswered
    std::atomic<int> s_delay_ms{0};       // the sync reply arrives after this long
    std::atomic<bool> p_mute{false};      // the pier-side query goes unanswered
    std::atomic<int> p_count{0};          // pier-side queries received
    std::atomic<int> s_count{0};          // sync and time commands received
    std::atomic<bool> gps_absent{false};  // no GPS module on the AUX bus: its version probe goes unanswered
    std::atomic<bool> gps_linked{false};  // GPS_LINKED (0x37) answer
    std::atomic<int> gps_lat_raw{0};      // GPS_GET_LAT: 24-bit signed fraction of 360 degrees
    std::atomic<int> gps_long_raw{0};     // GPS_GET_LONG: the same encoding
    std::atomic<int> order{0};            // arrival counter shared by the goto and cancel records below
    std::atomic<int> goto_last{0};        // arrival number of the last goto, GOTO or axis goto-fast (0: none)
    std::atomic<int> cancel_first{0};     // arrival number of the first goto cancel (0: none)
};

std::string snapshot_reply(SnapshotMount& st, const std::string& chunk) {
    st.transactions.fetch_add(1);
    if (chunk.empty()) return "0#";
    if (st.mute.load()) return "";
    if (st.latency_ms.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(st.latency_ms.load()));
    switch (chunk[0]) {
        case 'm':  // CGX-L: German equatorial
            return std::string(1, static_cast<char>(24)) + "#";
        case 'V':
            return std::string("\x05\x23#", 3);
        case 'w':
            st.w_count.fetch_add(1);
            if (st.w_fail.load()) return "";
            if (st.w_delay_ms.load() > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(st.w_delay_ms.load()));
            }
            return std::string(8, '\0') + "#";
        case 'e':
        case 'E':
            return st.shifted.load() ? "13AB0500,21000500#" : "12AB0500,20000500#";
        case 'z':
        case 'Z':
            if (st.z_fail.load()) return "";
            return "20AB0500,10000500#";
        case 'r':
        case 'R':
            st.goto_last.store(st.order.fetch_add(1) + 1);
            return "#";
        case 'L':
            if (st.l_fail.load()) return "";
            return st.slewing.load() ? "1#" : "0#";
        case 'J':
            return "1#";
        case 'p':
            st.slow_count.fetch_add(1);
            st.p_count.fetch_add(1);
            if (st.p_mute.load()) return "";
            return std::string(1, st.pier.load()) + "#";
        case 't':
            st.slow_count.fetch_add(1);
            return st.tracking.load() ? std::string("\x02#") : std::string("\x00#", 2);
        case 'T':
            st.t_count.fetch_add(1);
            if (st.t_delay_ms.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(st.t_delay_ms.load()));
            st.tracking.store(chunk.size() > 1 && chunk[1] != 0);
            return "#";
        case 'h': {  // the mount's clock: the host's UTC with a zero zone, so UTCDate has a mount time to seed from
            const std::time_t now = std::time(nullptr);
            std::tm utc{};
            gmtime_r(&now, &utc);
            std::string reply;
            for (const int field :
                 {utc.tm_hour, utc.tm_min, utc.tm_sec, utc.tm_mon + 1, utc.tm_mday, utc.tm_year % 100, 0, 0}) {
                reply.push_back(static_cast<char>(field));
            }
            return reply + "#";
        }
        case 'H':
            if (st.h_mute.load()) return "";
            return "#";
        case 'M':
            st.m_count.fetch_add(1);
            {
                int none = 0;
                st.cancel_first.compare_exchange_strong(none, st.order.fetch_add(1) + 1);
            }
            if (st.m_delay_ms.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(st.m_delay_ms.load()));
            return "#";
        case 'W':
        case 's':
        case 'S':
            st.s_count.fetch_add(1);
            if (st.s_delay_ms.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(st.s_delay_ms.load()));
            return "#";
        case 'P': {  // AUX passthrough: P len dev op ...
            const unsigned char op = chunk.size() > 3 ? static_cast<unsigned char>(chunk[3]) : 0;
            const unsigned char device = chunk.size() > 2 ? static_cast<unsigned char>(chunk[2]) : 0;
            if (device == 0xB0) {  // GPS module (AUX 0xB0)
                if (op == 0xFE) return st.gps_absent.load() ? std::string() : std::string("\x01\x00#", 3);
                if (op == 0x37) return std::string(1, st.gps_linked.load() ? '\x01' : '\x00') + "#";
                if (op == 0x01 || op == 0x02) {
                    const int raw = (op == 0x01 ? st.gps_lat_raw.load() : st.gps_long_raw.load()) & 0xFFFFFF;
                    return std::string{static_cast<char>((raw >> 16) & 0xFF), static_cast<char>((raw >> 8) & 0xFF),
                                       static_cast<char>(raw & 0xFF), '#'};
                }
            }
            if (op == 0xFE && device == 0x32) return std::string("\x01\x00#", 3);  // DEC autoguider port probe
            if (op == 0xFE) return std::string("\x00\x00#", 3);                    // absent bus devices
            if (op == 0x26) return "#";                                            // MC_AUX_GUIDE
            // MC_GOTO_FAST: a park whose target lies across the meridian (host-clock LST) flips with it
            if (op == 0x02) {
                st.goto_last.store(st.order.fetch_add(1) + 1);
                return "#";
            }
            if (op == 0x12) {                                                      // MC_LEVEL_DONE
                st.level_count.fetch_add(1);
                if (st.level_mute.load()) return "";
            }
            return std::string("\xFF#");
        }
        default:
            return "0#";
    }
}

std::unique_ptr<alpacacore::TelescopeDriver> snapshot_driver(alpacacore::test::FakeMountServer& server,
                                                             FakeTaskClock& clock) {
    cel::ConnectionInfo info;
    info.type = cel::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 300;
    auto driver = cel::create_celestron_telescope_with_site(0, info, -33.856784, 151.215297, 42.0, false,
                                                            cel::CelestronAlignmentSetting::Auto, clock);
    driver->set_connected(true);
    return driver;
}

bool snapshot_has_entry(const alpacacore::TelescopeDriver& driver, const std::string& name) {
    for (const auto& entry : driver.get_device_state()) {
        if (entry.name == name) return true;
    }
    return false;
}

}  // namespace

// #1408: the pulse body takes no driver mutex, but joining it waits out the serial transaction it is in.
// Joining with mutex_ held stalls every getter for that long. The mount is the snapshot fake, which answers
// every poll query with a valid frame, so the link cannot fault and let the case pass without the join.
TEST_CASE("Celestron Telescope Driver - PulseGuide joins the previous pulse without holding the driver mutex",
          "[celestron][telescope][unit][pulseguide]") {
    auto st = std::make_shared<SnapshotMount>();
    auto chunk_in_flight = std::make_shared<std::atomic<bool>>(false);
    alpacacore::test::FakeMountServer server([st, chunk_in_flight](const std::string& command) {
        // The chained 245 cs chunk of the long RA pulse: a slow serial transaction.
        if (command.size() > 5 && command[0] == 'P' && command[2] == '\x10' && command[3] == '\x26' &&
            static_cast<unsigned char>(command[5]) == 245) {
            chunk_in_flight->store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        return snapshot_reply(*st, command);
    });
    REQUIRE(server.ok());
    cel::ConnectionInfo info;
    info.type = cel::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 1000;  // above the 300 ms delay
    auto driver = cel::create_celestron_telescope(0, info);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    REQUIRE(driver->get_can_pulse_guide());

    driver->pulse_guide(2, 5000);  // East, RA: 255 cs now, 245 cs chained at ~2.55 s
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!chunk_in_flight->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(chunk_in_flight->load());

    std::atomic<bool> stop{false};
    std::atomic<long long> max_blocked_ms{0};
    std::thread sampler([&]() {
        while (!stop.load()) {
            const auto begin = std::chrono::steady_clock::now();
            (void)driver->get_is_pulse_guiding();
            const auto waited =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
            long long seen = max_blocked_ms.load();
            while (waited > seen && !max_blocked_ms.compare_exchange_weak(seen, waited)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Same axis: must cancel and join the body while its chunk is in flight.
    bool second_threw = false;
    try {
        driver->pulse_guide(2, 1000);
    } catch (const std::exception&) {
        second_threw = true;
    }
    stop.store(true);
    sampler.join();

    CHECK_FALSE(second_threw);
    INFO("longest get_is_pulse_guiding wait " << max_blocked_ms.load() << " ms");
    CHECK(max_blocked_ms.load() < 100);
    driver->set_connected(false);
}

// #1408: a refused chain start leaves the first chunk running on the mount; the driver stops the axis as
// AbortSlew does and drops the pulse state before the refusal reaches the client.
TEST_CASE("Celestron Telescope Driver - a refused PulseGuide chain start stops the axis and clears the pulse",
          "[celestron][telescope][unit][pulseguide]") {
    auto st = std::make_shared<SnapshotMount>();
    auto commands = std::make_shared<std::vector<std::string>>();
    auto commands_mutex = std::make_shared<std::mutex>();
    alpacacore::test::FakeMountServer server([st, commands, commands_mutex](const std::string& command) {
        {
            std::lock_guard<std::mutex> lock(*commands_mutex);
            commands->push_back(command);
        }
        return snapshot_reply(*st, command);
    });
    REQUIRE(server.ok());
    cel::ConnectionInfo info;
    info.type = cel::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 1000;
    auto driver = cel::create_celestron_telescope(0, info);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    cel::set_pulse_spawn_for_testing(*driver, [](std::function<void()>) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    {
        std::lock_guard<std::mutex> lock(*commands_mutex);
        commands->clear();
    }
    CHECK_THROWS_AS(driver->pulse_guide(2, 5000), alpacacore::AlpacaException);  // 5 s needs a chained chunk

    const auto sent = [&](const std::string& prefix) {
        std::lock_guard<std::mutex> lock(*commands_mutex);
        for (const auto& c : *commands) {
            if (c.compare(0, prefix.size(), prefix) == 0) return true;
        }
        return false;
    };
    CHECK(sent(std::string("P\x03\x10\x26", 4)));  // the first guide chunk
    const bool stop = sent(std::string("P\x02\x10\x24\x00", 5));
    CHECK(stop);  // then the stop
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

TEST_CASE("Celestron snapshot - getters and DeviceState make no mount transaction at 100 ms per transaction",
          "[celestron][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    // The poll is parked on the clock once its first tick (the connect's reads) is over. The old driver has no
    // poll, so this wait only times out there.
    (void)clock.wait_for_waiters(1, std::chrono::milliseconds(1000));
    // Past the old 2 s position cache: a getter that still ran its own TTL would ask the mount from here on.
    clock.advance(std::chrono::seconds(3));
    (void)clock.wait_for_woken_settled(std::chrono::milliseconds(2000));

    st->latency_ms = static_cast<int>(kSnapshotLatency.count());
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
        {"DeviceState", [&] { (void)driver->get_device_state(); }},
    };
    for (const auto& [name, read] : reads) {
        CAPTURE(name);
        const int transactions = st->transactions.load();
        CHECK(timed(read) < kSnapshotGetterBudget);
        CHECK(st->transactions.load() == transactions);
    }
    CHECK(st->transactions.load() == before);
    // DeviceState drops a getter that throws, so a read that quietly failed would still be fast: name what must be in
    // it.
    for (const char* name :
         {"RightAscension", "Declination", "Altitude", "Azimuth", "Slewing", "Tracking", "SideOfPier", "TimeStamp"}) {
        CAPTURE(name);
        CHECK(snapshot_has_entry(*driver, name));
    }
    driver->set_connected(false);
}

TEST_CASE("Celestron snapshot - a setter's value is read back at once and a stale poll does not undo it",
          "[celestron][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    REQUIRE(driver->get_tracking());

    driver->set_tracking(false);
    CHECK_FALSE(driver->get_tracking());
    driver->set_tracking(true);
    CHECK(driver->get_tracking());

    // A sync reads back at once, with no poll in between.
    driver->sync_to_coordinates(3.25, 41.5);
    CHECK(std::abs(driver->get_right_ascension() - 3.25) < 1e-3);
    CHECK(std::abs(driver->get_declination() - 41.5) < 1e-3);

    // A poll that sampled the mount before the write must not publish over it: the write lands between that
    // poll's sample and its publish.
    cel::set_status_poll_after_sample_hook_for_testing(*driver, [&driver] { driver->set_tracking(false); });
    CHECK(cel::poll_status_now_for_testing(*driver));
    cel::set_status_poll_after_sample_hook_for_testing(*driver, nullptr);
    CHECK_FALSE(driver->get_tracking());
    // The next poll measures the mount, which the write reached.
    CHECK(cel::poll_status_now_for_testing(*driver));
    CHECK_FALSE(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("Celestron snapshot - a position the mount moved is served after the next poll, stamped when measured",
          "[celestron][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);

    const double ra_before = driver->get_right_ascension();
    st->shifted = true;
    CHECK(std::abs(driver->get_right_ascension() - ra_before) < 1e-9);  // served from memory until the poll
    CHECK(cel::poll_status_now_for_testing(*driver));
    CHECK(std::abs(driver->get_right_ascension() - ra_before) > 1e-3);

    // The TimeStamp names when the poll measured, not when DeviceState was asked.
    clock.advance(std::chrono::seconds(2));
    const auto age = std::chrono::system_clock::now() - parse_timestamp(time_stamp_of(*driver));
    CHECK(age > std::chrono::milliseconds(1500));
    CHECK(age < std::chrono::milliseconds(4000));
    driver->set_connected(false);
}

TEST_CASE("Celestron snapshot - a site written during the poll's location read is not overwritten",
          "[celestron][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    cel::ConnectionInfo info;
    info.type = cel::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 300;
    st->w_fail = true;  // the connect-time location read fails: the site is not valid yet
    auto driver = cel::create_celestron_telescope_with_site(0, info, std::nullopt, std::nullopt, std::nullopt, false,
                                                            cel::CelestronAlignmentSetting::Auto, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    CHECK_THROWS_AS(driver->get_site_latitude(), alpacacore::AlpacaException);

    st->w_fail = false;
    st->w_delay_ms = 200;
    clock.advance(std::chrono::seconds(31));  // the poll's site retry delay has passed
    const int w_before = st->w_count.load();
    std::thread poll([&] { (void)cel::poll_status_now_for_testing(*driver); });
    // The write must land while the poll's location read is in flight: wait for the mount to receive it.
    for (int i = 0; i < 500 && st->w_count.load() == w_before; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(st->w_count.load() > w_before);
    driver->set_site_latitude(-33.856784);
    poll.join();
    CHECK(std::abs(driver->get_site_latitude() - (-33.856784)) < 1e-6);
    driver->set_connected(false);
}

TEST_CASE("Celestron snapshot - Slewing is unknown, not false, until a query has answered",
          "[celestron][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    st->l_fail = true;  // the connect-time slewing query goes unanswered
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    CHECK_NOTHROW(driver->get_right_ascension());  // the position is known
    try {
        (void)driver->get_slewing();
        FAIL("an unanswered slewing query must not be served as false");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(e.what()).find("not available yet") != std::string::npos);
    }
    st->l_fail = false;
    CHECK(cel::poll_status_now_for_testing(*driver));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("Celestron snapshot - the slow fields of a tick stop at the first failure",
          "[celestron][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    st->z_fail = true;  // alt-az times out; pier side and tracking mode would each wait out their own timeout
    st->slow_count = 0;
    (void)cel::poll_status_now_for_testing(*driver);  // tick 0 is a full tick
    CHECK(st->slow_count.load() == 0);
    st->z_fail = false;
    (void)cel::poll_status_now_for_testing(*driver);
    CHECK(st->slow_count.load() > 0);  // the same tick asks for them when alt-az answers
    driver->set_connected(false);
}

TEST_CASE("Celestron snapshot - a failed location read ends the tick", "[celestron][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    cel::ConnectionInfo info;
    info.type = cel::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 200;
    st->w_fail = true;  // the location read fails at connect and again in the poll
    auto driver = cel::create_celestron_telescope_with_site(0, info, std::nullopt, std::nullopt, std::nullopt, false,
                                                            cel::CelestronAlignmentSetting::Auto, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    clock.advance(std::chrono::seconds(31));  // the poll's site retry delay has passed
    const int before = st->transactions.load();
    (void)cel::poll_status_now_for_testing(*driver);  // tick 0 is a full tick, so the site read is due
    // A dead link would otherwise cost the location timeout and then the sample's own timeout, past one exchange.
    CHECK(st->transactions.load() - before == 1);
    driver->set_connected(false);
}

TEST_CASE("Celestron snapshot - a link whose location never reads still samples on the next full tick",
          "[celestron][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    cel::ConnectionInfo info;
    info.type = cel::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 200;
    st->w_fail = true;  // the location never reads
    auto driver = cel::create_celestron_telescope_with_site(0, info, std::nullopt, std::nullopt, std::nullopt, false,
                                                            cel::CelestronAlignmentSetting::Auto, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    clock.advance(std::chrono::seconds(31));
    (void)cel::poll_status_now_for_testing(*driver);  // the site read fails and ends this tick
    clock.advance(std::chrono::seconds(4));           // the next full tick
    const int before = st->transactions.load();
    (void)cel::poll_status_now_for_testing(*driver);
    // The site is not retried yet, so this tick samples instead of paying another location timeout.
    CHECK(st->transactions.load() - before > 1);
    driver->set_connected(false);
}

TEST_CASE("Celestron snapshot - past the staleness bound DeviceState is empty with no TimeStamp",
          "[celestron][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    CHECK(snapshot_has_entry(*driver, "TimeStamp"));
    const double ra_at_connect = driver->get_right_ascension();

    st->mute = true;
    for (int i = 0; i < 3; ++i) (void)cel::poll_status_now_for_testing(*driver);
    CHECK_FALSE(driver->get_link_fault().empty());
    CHECK(driver->get_connected());  // a young fault keeps Connected

    clock.advance(alpacacore::util::kLinkStalenessBound + std::chrono::seconds(1));
    (void)cel::poll_status_now_for_testing(*driver);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_device_state().empty());
    CHECK_FALSE(snapshot_has_entry(*driver, "TimeStamp"));
    try {
        (void)driver->get_right_ascension();
        FAIL("a lost link must refuse the read");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
    }
    st->mute = false;

    // Decision 0009 point 4: Connected=true against a lost link relinks, on both entry points.
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(std::abs(driver->get_right_ascension() - ra_at_connect) < 1e-9);

    st->mute = true;
    for (int i = 0; i < 3; ++i) (void)cel::poll_status_now_for_testing(*driver);
    clock.advance(alpacacore::util::kLinkStalenessBound + std::chrono::seconds(1));
    (void)cel::poll_status_now_for_testing(*driver);
    REQUIRE_FALSE(driver->get_connected());
    st->mute = false;
    driver->connect();  // the async route
    CHECK(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(driver->get_connected());
    CHECK(std::abs(driver->get_right_ascension() - ra_at_connect) < 1e-9);
    driver->set_connected(false);
}

// The UTCDate readback is the client's value plus the host time elapsed since the write, and that elapsed time is read
// from the driver's TaskClock (decision 0014), so the case moves it with advance() and never sleeps.
TEST_CASE("Celestron UTCDate - a written UTCDate reads back advanced by the task clock and is dropped on reconnect",
          "[celestron][telescope][snapshot][utc-offset]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    (void)clock.wait_for_waiters(1, std::chrono::milliseconds(1000));

    // An hour from the host, with a sub-second part, so neither the re-seed after a reconnect nor a truncating
    // readback can equal it by coincidence.
    const auto written = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() +
                                                                                 std::chrono::hours(1)) +
                         std::chrono::milliseconds(250);
    driver->set_utc_date(written);
    CHECK(driver->get_utc_date() - written < std::chrono::seconds(2));

    clock.advance(std::chrono::seconds(60));
    (void)clock.wait_for_woken_settled(std::chrono::milliseconds(2000));
    REQUIRE(driver->get_connected());
    const auto advanced = driver->get_utc_date();
    CHECK(advanced >= written + std::chrono::seconds(60));
    CHECK(advanced < written + std::chrono::seconds(62));

    driver->set_connected(false);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const auto reseeded = driver->get_utc_date();
    const auto gap = reseeded > written ? reseeded - written : written - reseeded;
    CHECK(gap > std::chrono::minutes(30));
    driver->set_connected(false);
}

// Decision 0016 rule 1 (#1437): a control-path exchange runs under the link's I/O lock only, never under the
// driver mutex_ a getter takes. Each case parks one exchange in the fake mount and times the getters meanwhile.
namespace {

bool snapshot_wait_for(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// A no-device getter and a snapshot getter, each held to the budget a fake-mount exchange would overrun.
void check_getters_do_not_wait(alpacacore::TelescopeDriver& driver) {
    CHECK(timed([&] { (void)driver.get_name(); }) < kSnapshotGetterBudget);
    CHECK(timed([&] { (void)driver.get_can_find_home(); }) < kSnapshotGetterBudget);
    CHECK(timed([&] { (void)driver.get_right_ascension(); }) < kSnapshotGetterBudget);
}

}  // namespace

TEST_CASE("Celestron FindHome - a completion check on a silent link does not hold the getters",
          "[celestron][telescope][snapshot][iolock]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    driver->find_home();
    st->level_mute = true;

    std::thread tick([&] { (void)cel::poll_status_now_for_testing(*driver); });
    REQUIRE(snapshot_wait_for([&] { return st->level_count.load() > 0; }));
    check_getters_do_not_wait(*driver);
    tick.join();
    driver->set_connected(false);
}

TEST_CASE("Celestron FindHome - the third failed check still sends the stops and no getter waits for them",
          "[celestron][telescope][snapshot][iolock]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    driver->find_home();
    st->level_mute = true;
    CHECK(cel::poll_status_now_for_testing(*driver));
    CHECK(cel::poll_status_now_for_testing(*driver));
    CHECK(st->m_count.load() == 0);  // two failures: no stop yet

    st->m_delay_ms = 600;
    std::thread tick([&] { (void)cel::poll_status_now_for_testing(*driver); });
    REQUIRE(snapshot_wait_for([&] { return st->m_count.load() > 0; }));  // the safety stop went out
    check_getters_do_not_wait(*driver);
    tick.join();
    CHECK_THROWS_WITH(driver->get_slewing(), Catch::Matchers::ContainsSubstring("FindHome completion polling failed"));
    driver->set_connected(false);
}

TEST_CASE("Celestron setters - a slow set_tracking exchange does not hold the getters",
          "[celestron][telescope][snapshot][iolock]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);

    st->t_delay_ms = 200;  // under the 300 ms exchange timeout: the write succeeds
    const int writes = st->t_count.load();
    std::thread setter([&] { driver->set_tracking(false); });
    REQUIRE(snapshot_wait_for([&] { return st->t_count.load() > writes; }));
    check_getters_do_not_wait(*driver);
    setter.join();
    CHECK_FALSE(driver->get_tracking());
    driver->set_connected(false);
}

namespace {

// Runs `op` on its own thread with every mount exchange slowed to kSlowExchange (under the 300 ms exchange timeout, so
// each succeeds), waits until its first exchange is on the wire and checks the getters meanwhile. A getter that waited
// behind the exchange would pay the whole of kSlowExchange.
constexpr int kSlowExchangeMs = 150;

// The driver runs on the fake task clock, so an op that waits on it (the blocking slew) gets virtual seconds from the
// loop below once the getters have been checked.
template <typename Op>
void check_getters_during(FakeTaskClock& clock, SnapshotMount& st, alpacacore::TelescopeDriver& driver, Op&& op) {
    st.latency_ms = kSlowExchangeMs;
    const int before = st.transactions.load();
    std::atomic<bool> done{false};
    std::thread worker([&] {
        try {
            op();
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the case checks the getters, not the op
        }
        done.store(true);
    });
    REQUIRE(snapshot_wait_for([&] { return st.transactions.load() > before; }));
    check_getters_do_not_wait(driver);
    while (!done.load()) {
        clock.advance(std::chrono::seconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    worker.join();
    st.latency_ms = 0;
}

struct SlowMountDriver {
    FakeTaskClock clock;
    std::shared_ptr<SnapshotMount> st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server{[this](const std::string& c) { return snapshot_reply(*st, c); }};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    SlowMountDriver() {
        REQUIRE(server.ok());
        driver = snapshot_driver(server, clock);
        REQUIRE(driver->get_connected());
        cel::set_status_poll_paused_for_testing(*driver, true);
    }
    ~SlowMountDriver() { driver->set_connected(false); }
};

}  // namespace

TEST_CASE("Celestron connect - a slow handshake does not hold Connected or Name",
          "[celestron][telescope][snapshot][iolock]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    cel::ConnectionInfo info;
    info.type = cel::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 300;
    auto driver = cel::create_celestron_telescope_with_site(0, info, -33.856784, 151.215297, 42.0, false,
                                                            cel::CelestronAlignmentSetting::Auto, clock);
    st->latency_ms = kSlowExchangeMs;
    std::thread connecting([&] { driver->set_connected(true); });
    REQUIRE(snapshot_wait_for([&] { return st->transactions.load() > 0; }));
    CHECK(timed([&] { CHECK_FALSE(driver->get_connected()); }) < kSnapshotGetterBudget);
    CHECK(timed([&] { (void)driver->get_name(); }) < kSnapshotGetterBudget);
    CHECK(timed([&] { (void)driver->get_can_find_home(); }) < kSnapshotGetterBudget);
    connecting.join();
    st->latency_ms = 0;
    CHECK(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    driver->set_connected(false);
}

TEST_CASE("Celestron disconnect - a close behind an exchange in flight does not hold the getters",
          "[celestron][telescope][snapshot][iolock]") {
    SlowMountDriver mount;
    mount.st->latency_ms = kSlowExchangeMs;
    const int before = mount.st->transactions.load();
    std::thread setter([&] {
        try {
            mount.driver->set_tracking(false);
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the case checks the getters
        }
    });
    REQUIRE(snapshot_wait_for([&] { return mount.st->transactions.load() > before; }));
    std::atomic<bool> close_started{false};
    std::thread closing([&] {
        close_started.store(true);
        mount.driver->set_connected(false);
    });
    REQUIRE(snapshot_wait_for([&] { return close_started.load(); }));
    CHECK(timed([&] { (void)mount.driver->get_name(); }) < kSnapshotGetterBudget);
    CHECK(timed([&] { (void)mount.driver->get_connected(); }) < kSnapshotGetterBudget);
    setter.join();
    closing.join();
    mount.st->latency_ms = 0;
    CHECK_FALSE(mount.driver->get_connected());
}

TEST_CASE("Celestron SlewToCoordinatesAsync - the initiator and its goto do not hold the getters",
          "[celestron][telescope][snapshot][iolock]") {
    SlowMountDriver mount;
    check_getters_during(mount.clock, *mount.st, *mount.driver,
                         [&] { mount.driver->slew_to_coordinates_async(5.0, 20.0); });
}

TEST_CASE("Celestron SlewToCoordinates - the blocking initiator does not hold the getters",
          "[celestron][telescope][snapshot][iolock]") {
    SlowMountDriver mount;
    check_getters_during(mount.clock, *mount.st, *mount.driver, [&] { mount.driver->slew_to_coordinates(5.0, 20.0); });
}

TEST_CASE("Celestron Park - the initiator and the park slew do not hold the getters",
          "[celestron][telescope][snapshot][iolock]") {
    SlowMountDriver mount;
    check_getters_during(mount.clock, *mount.st, *mount.driver, [&] { mount.driver->park(); });
}

TEST_CASE("Celestron SyncToCoordinates - the sync exchange does not hold the getters",
          "[celestron][telescope][snapshot][iolock]") {
    SlowMountDriver mount;
    check_getters_during(mount.clock, *mount.st, *mount.driver, [&] { mount.driver->sync_to_coordinates(5.0, 20.0); });
}

TEST_CASE("Celestron FindHome - the dispatch does not hold the getters", "[celestron][telescope][snapshot][iolock]") {
    SlowMountDriver mount;
    check_getters_during(mount.clock, *mount.st, *mount.driver, [&] { mount.driver->find_home(); });
}

TEST_CASE("Celestron MoveAxis - the rate write does not hold the getters", "[celestron][telescope][snapshot][iolock]") {
    SlowMountDriver mount;
    check_getters_during(mount.clock, *mount.st, *mount.driver, [&] { mount.driver->move_axis(0, 1.0); });
}

TEST_CASE("Celestron AbortSlew - the stops do not hold the getters", "[celestron][telescope][snapshot][iolock]") {
    SlowMountDriver mount;
    check_getters_during(mount.clock, *mount.st, *mount.driver, [&] { mount.driver->abort_slew(); });
}

TEST_CASE("Celestron PulseGuide - the pier read and the guide write do not hold the getters",
          "[celestron][telescope][snapshot][iolock]") {
    SlowMountDriver mount;
    REQUIRE(mount.driver->get_can_pulse_guide());
    check_getters_during(mount.clock, *mount.st, *mount.driver, [&] { mount.driver->pulse_guide(0, 500); });
}

TEST_CASE("Celestron site and time writes - the write exchange does not hold the getters",
          "[celestron][telescope][snapshot][iolock]") {
    SlowMountDriver mount;
    SECTION("SiteLatitude") {
        check_getters_during(mount.clock, *mount.st, *mount.driver,
                             [&] { mount.driver->set_site_latitude(-33.856784); });
    }
    SECTION("SiteLongitude") {
        check_getters_during(mount.clock, *mount.st, *mount.driver,
                             [&] { mount.driver->set_site_longitude(151.215297); });
    }
    SECTION("UTCDate") {
        check_getters_during(mount.clock, *mount.st, *mount.driver,
                             [&] { mount.driver->set_utc_date(std::chrono::system_clock::now()); });
    }
    SECTION("CommandString") {
        check_getters_during(mount.clock, *mount.st, *mount.driver,
                             [&] { (void)mount.driver->command_string("V", true); });
    }
    SECTION("GuideRate") {
        check_getters_during(mount.clock, *mount.st, *mount.driver,
                             [&] { mount.driver->set_guide_rate(alpacacore::GuideRate{0.002, 0.002}); });
    }
}

// The FindHome failure stops go out only while no newer initiator has taken the mount. The hook stands in for the
// client that saw the sticky FindHome error and sent a new goto before the stops: the poll then skips them.
TEST_CASE("Celestron FindHome - the failure stops are skipped when a new goto took the mount first",
          "[celestron][telescope][snapshot][iolock]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    driver->find_home();
    st->level_mute = true;
    CHECK(cel::poll_status_now_for_testing(*driver));
    CHECK(cel::poll_status_now_for_testing(*driver));
    REQUIRE(st->m_count.load() == 0);

    int m_after_goto = -1;
    cel::set_failure_stop_hook_for_testing(*driver, [&] {
        driver->slew_to_coordinates_async(1.5, 20.0);
        m_after_goto = st->m_count.load();
    });
    (void)cel::poll_status_now_for_testing(*driver);
    cel::set_failure_stop_hook_for_testing(*driver, nullptr);
    REQUIRE(m_after_goto >= 0);
    CHECK(st->m_count.load() == m_after_goto);  // no stale goto cancel after the new goto
    driver->set_connected(false);
}

// A sync holds the initiator lock without taking a motion generation. The third-failure stops that find the lock busy
// stay pending and go out on a later poll tick, once the sync is done.
TEST_CASE("Celestron FindHome - the failure stops still go out when a sync held the initiator lock",
          "[celestron][telescope][snapshot][iolock]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    driver->find_home();
    st->level_mute = true;
    CHECK(cel::poll_status_now_for_testing(*driver));
    CHECK(cel::poll_status_now_for_testing(*driver));
    REQUIRE(st->m_count.load() == 0);

    st->s_delay_ms = 300;
    std::thread syncer;
    cel::set_failure_stop_hook_for_testing(*driver, [&] {
        if (!syncer.joinable()) {
            const int before = st->s_count.load();
            syncer = std::thread([&] { driver->sync_to_coordinates(1.5, 20.0); });
            REQUIRE(snapshot_wait_for([&] { return st->s_count.load() > before; }));  // the sync owns initiator_mutex_
        }
    });
    (void)cel::poll_status_now_for_testing(*driver);  // third failure: the lock is busy, the stop stays pending
    cel::set_failure_stop_hook_for_testing(*driver, nullptr);
    REQUIRE(syncer.joinable());
    syncer.join();
    for (int i = 0; i < 3; ++i) {
        (void)cel::poll_status_now_for_testing(*driver);
    }
    CHECK(st->m_count.load() >= 1);
    driver->set_connected(false);
}

// Unpark with no park in flight sends nothing to the mount and takes no motion generation, so a failure stop that a
// sync deferred stays pending through it and still goes out on a later poll tick.
TEST_CASE("Celestron FindHome - the failure stops still go out after an Unpark",
          "[celestron][telescope][snapshot][iolock]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    driver->find_home();
    st->level_mute = true;
    CHECK(cel::poll_status_now_for_testing(*driver));
    CHECK(cel::poll_status_now_for_testing(*driver));

    st->s_delay_ms = 300;
    std::thread syncer;
    cel::set_failure_stop_hook_for_testing(*driver, [&] {
        if (!syncer.joinable()) {
            const int before = st->s_count.load();
            syncer = std::thread([&] { driver->sync_to_coordinates(1.5, 20.0); });
            REQUIRE(snapshot_wait_for([&] { return st->s_count.load() > before; }));  // the sync owns initiator_mutex_
        }
    });
    (void)cel::poll_status_now_for_testing(*driver);  // third failure: the lock is busy, the stop stays pending
    cel::set_failure_stop_hook_for_testing(*driver, nullptr);
    REQUIRE(syncer.joinable());
    syncer.join();
    REQUIRE(st->m_count.load() == 0);
    driver->unpark();
    for (int i = 0; i < 3; ++i) {
        (void)cel::poll_status_now_for_testing(*driver);
    }
    CHECK(st->m_count.load() >= 1);
    driver->set_connected(false);
}

// SlewToCoordinatesAsync takes a motion generation before its pier-side read. When that read fails no goto went out,
// so a failure stop deferred meanwhile stays pending, through a poll tick in the window too, and still goes out.
TEST_CASE("Celestron FindHome - the failure stops still go out when an async goto fails before it is sent",
          "[celestron][telescope][snapshot][iolock]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    driver->find_home();
    st->level_mute = true;
    CHECK(cel::poll_status_now_for_testing(*driver));
    CHECK(cel::poll_status_now_for_testing(*driver));

    st->p_mute = true;  // the pier-side read times out, before the goto
    std::thread slewer;
    std::atomic<bool> refused{false};
    cel::set_failure_stop_hook_for_testing(*driver, [&] {
        if (!slewer.joinable()) {
            const int before = st->p_count.load();
            slewer = std::thread([&] {
                try {
                    driver->slew_to_coordinates_async(1.5, 20.0);
                } catch (const std::exception&) {
                    refused = true;
                }
            });
            CHECK(snapshot_wait_for([&] { return st->p_count.load() > before; }));  // the slew owns the mount
        }
    });
    (void)cel::poll_status_now_for_testing(*driver);  // third failure: the lock is busy, the stop stays pending
    cel::set_failure_stop_hook_for_testing(*driver, nullptr);
    (void)cel::poll_status_now_for_testing(*driver);  // a tick while the read is out keeps it pending
    REQUIRE(slewer.joinable());
    slewer.join();
    REQUIRE(refused.load());
    REQUIRE(st->m_count.load() == 0);
    for (int i = 0; i < 3; ++i) {
        (void)cel::poll_status_now_for_testing(*driver);
    }
    CHECK(st->m_count.load() >= 1);
    driver->set_connected(false);
}

// A poll tick reads the pending stop while the async goto still owns the mount. The goto then fails before it is sent
// and hands the stop to its own generation, and a sync queued behind it takes the lock before the tick's try_lock: the
// tick finds the lock busy again but must not move the stop back to the older generation, which the next free tick
// would drop as superseded.
TEST_CASE("Celestron FindHome - the failure stops still go out when a sync follows an async goto that was not sent",
          "[celestron][telescope][snapshot][iolock]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    driver->find_home();
    st->level_mute = true;
    CHECK(cel::poll_status_now_for_testing(*driver));
    CHECK(cel::poll_status_now_for_testing(*driver));

    st->p_mute = true;     // the pier-side read times out, before the goto
    st->s_delay_ms = 300;  // the sync holds the initiator lock through the retry's try_lock
    std::thread slewer;
    std::thread syncer;
    std::atomic<bool> refused{false};
    int calls = 0;
    int s_before = 0;
    cel::set_failure_stop_hook_for_testing(*driver, [&] {
        ++calls;
        if (calls == 1) {  // the third failure
            const int p_before = st->p_count.load();
            slewer = std::thread([&] {
                try {
                    driver->slew_to_coordinates_async(1.5, 20.0);
                } catch (const std::exception&) {
                    refused = true;
                }
            });
            CHECK(snapshot_wait_for([&] { return st->p_count.load() > p_before; }));  // the slew owns the mount
            s_before = st->s_count.load();
            syncer = std::thread([&] {
                try {
                    driver->sync_to_coordinates(1.5, 20.0);  // queues on the initiator lock
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): only its hold on the lock counts
                }
            });
        } else if (calls == 2) {  // the retry tick, which read the stop before the hand-back
            CHECK(snapshot_wait_for([&] { return refused.load(); }));                 // the stop was handed back
            CHECK(snapshot_wait_for([&] { return st->s_count.load() > s_before; }));  // the sync owns the lock
        }
    });
    (void)cel::poll_status_now_for_testing(*driver);  // third failure: the lock is busy, the stop stays pending
    (void)cel::poll_status_now_for_testing(*driver);  // the retry finds the lock busy again
    cel::set_failure_stop_hook_for_testing(*driver, nullptr);
    REQUIRE(calls == 2);
    REQUIRE(slewer.joinable());
    REQUIRE(syncer.joinable());
    slewer.join();
    syncer.join();
    REQUIRE(refused.load());
    REQUIRE(st->m_count.load() == 0);
    for (int i = 0; i < 3; ++i) {
        (void)cel::poll_status_now_for_testing(*driver);
    }
    CHECK(st->m_count.load() >= 1);
    driver->set_connected(false);
}

// Park takes a motion generation before its body starts. When the slot refuses the start and no body was displaced no
// goto went out, so a failure stop deferred meanwhile stays pending, through a poll tick in the window too.
TEST_CASE("Celestron FindHome - the failure stops still go out when Park cannot start its body",
          "[celestron][telescope][snapshot][iolock]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    driver->find_home();
    st->level_mute = true;
    CHECK(cel::poll_status_now_for_testing(*driver));
    CHECK(cel::poll_status_now_for_testing(*driver));

    std::atomic<bool> in_spawn{false};
    std::atomic<bool> release{false};
    cel::set_slew_spawn_for_testing(*driver, [&](std::function<void()>) -> std::thread {
        in_spawn = true;  // Park owns the mount
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    std::thread parker;
    std::atomic<bool> refused{false};
    cel::set_failure_stop_hook_for_testing(*driver, [&] {
        if (!parker.joinable()) {
            parker = std::thread([&] {
                try {
                    driver->park();
                } catch (const std::exception&) {
                    refused = true;
                }
            });
            CHECK(snapshot_wait_for([&] { return in_spawn.load(); }));
        }
    });
    (void)cel::poll_status_now_for_testing(*driver);  // third failure: the lock is busy, the stop stays pending
    cel::set_failure_stop_hook_for_testing(*driver, nullptr);
    (void)cel::poll_status_now_for_testing(*driver);  // a tick while the start is held keeps it pending
    release = true;
    REQUIRE(parker.joinable());
    parker.join();
    REQUIRE(refused.load());
    REQUIRE(st->m_count.load() == 0);
    for (int i = 0; i < 3; ++i) {
        (void)cel::poll_status_now_for_testing(*driver);
    }
    CHECK(st->m_count.load() >= 1);
    driver->set_connected(false);
}

// A connect whose post-open init throws (the time write goes unanswered) closes the link again and leaves
// Connected false, so a retry opens it afresh.
TEST_CASE("Celestron connect - a failed time write closes the link and leaves Connected false",
          "[celestron][telescope][snapshot][iolock]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    cel::ConnectionInfo info;
    info.type = cel::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 300;
    auto driver = cel::create_celestron_telescope_with_site(0, info, -33.856784, 151.215297, 42.0, true,
                                                            cel::CelestronAlignmentSetting::Auto, clock);
    st->h_mute = true;
    CHECK_THROWS(driver->set_connected(true));
    CHECK_FALSE(driver->get_connected());
    st->h_mute = false;
    driver->set_connected(true);
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

// A slewing read taken with mutex_ free is not applied after a newer motion command took the mount. The hook stands in
// for the client whose AbortSlew lands between the read and its publish: the read still says "slewing", and Slewing
// must stay false.
TEST_CASE("Celestron Slewing read - a read that lands after AbortSlew is not applied",
          "[celestron][telescope][snapshot][iolock]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    st->slewing = true;
    bool aborted = false;
    cel::set_slewing_read_hook_for_testing(*driver, [&] {
        if (!aborted) {
            aborted = true;
            driver->abort_slew();
        }
    });
    std::atomic<bool> done{false};
    std::thread slewer([&] {
        try {
            driver->slew_to_coordinates(1.5, 20.0);
        } catch (const std::exception&) {
            // superseded by the abort: the blocking slew ends with an error
        }
        done.store(true);
    });
    for (int i = 0; i < 400 && !done.load(); ++i) {  // past the forced-slewing window, to the first real read
        clock.wait_for_waiters(1, std::chrono::milliseconds(100));
        clock.advance(std::chrono::milliseconds(250));
        clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
    slewer.join();
    cel::set_slewing_read_hook_for_testing(*driver, nullptr);
    REQUIRE(aborted);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// AbortSlew claims the slot instead of joining the park body, so the park body that was held before its GOTO finds
// itself superseded at the send gate and sends nothing: the cancel is the only motion command that reaches the mount.
// The hook holds the park body past its last supersede check, before the gate, until the cancel reaches the mount or
// 500 ms pass. A GOTO that was already on the wire would have to land before the cancel (the gate), never after it.
TEST_CASE("Celestron AbortSlew - the cancel reaches the mount and the held park GOTO never follows it",
          "[celestron][telescope][snapshot][iolock]") {
    SlowMountDriver mount;
    std::atomic<bool> in_dispatch{false};
    cel::set_slew_dispatch_hook_for_testing(*mount.driver, [&] {
        in_dispatch = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (mount.st->m_count.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    mount.driver->park();
    REQUIRE(snapshot_wait_for([&] { return in_dispatch.load(); }));
    try {
        mount.driver->abort_slew();
    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the case checks the exchange order
    }
    cel::set_slew_dispatch_hook_for_testing(*mount.driver, nullptr);
    mount.driver->set_connected(false);  // joins the park body: it has seen the claim by now
    REQUIRE(mount.st->cancel_first.load() > 0);
    CHECK(mount.st->goto_last.load() == 0);  // the park body lost the slot before the gate: no GOTO at all
}

#endif  // _WIN32

#ifndef _WIN32

// ── #1478 spec-audit items ───────────────────────────────────────────────────

TEST_CASE("Celestron spec audit - DestinationSideOfPier rejects out-of-range RA and Dec",
          "[celestron][telescope][unit][spec-audit]") {
    alpacacore::test::FakeMountServer server([](const std::string& command) {
        if (const auto idle = pulse_mount_idle_reply(command)) return *idle;
        return std::string("00000000,00000000#");
    });
    REQUIRE(server.ok());
    auto driver = connect_pulse_guide_mount(server);
    // ITelescopeV4.DestinationSideOfPier: InvalidValueException for RA outside 0..24 h or Dec outside -90..90.
    require_alpaca_error([&] { (void)driver->get_destination_side_of_pier(25.0, 0.0); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)driver->get_destination_side_of_pier(-1.0, 0.0); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)driver->get_destination_side_of_pier(12.0, 91.0); },
                         alpacacore::AlpacaError::InvalidValue);
    driver->set_connected(false);
}

TEST_CASE("Celestron spec audit - SiteElevation never set throws InvalidOperation",
          "[celestron][telescope][unit][spec-audit]") {
    alpacacore::test::FakeMountServer server([](const std::string& command) {
        if (const auto idle = pulse_mount_idle_reply(command)) return *idle;
        return std::string("00000000,00000000#");
    });
    REQUIRE(server.ok());
    auto driver = connect_pulse_guide_mount(server);
    // ASCOM time-and-place FAQ: reading a site value that was never set throws InvalidOperationException.
    require_alpaca_error([&] { (void)driver->get_site_elevation(); }, alpacacore::AlpacaError::InvalidOperation);
    driver->set_connected(false);
}

TEST_CASE("Celestron spec audit - UTCDate throws InvalidOperation when the mount clock cannot be read",
          "[celestron][telescope][unit][spec-audit]") {
    // Only the position queries get the canned pair. Every other reply is a bare terminator, so no leftover bytes
    // of a longer reply sit in the socket and answer the clock read ('h') as a time; the clock read gets no time.
    std::atomic<int> clock_reads{0};
    alpacacore::test::FakeMountServer server([&clock_reads](const std::string& command) {
        if (const auto idle = pulse_mount_idle_reply(command)) return *idle;
        if (command == "e" || command == "z") return std::string("00000000,00000000#");
        if (command == "h") ++clock_reads;
        return std::string("#");
    });
    REQUIRE(server.ok());
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 100;
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    // Decision 0014 item 4 leaves seeding in the driver; the host clock must not stand in for the mount's.
    require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
    // Getters never wait on device I/O: the failed connect-time read is remembered, so a poll sends no further 'h'.
    const int reads_after_first = clock_reads.load();
    require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
    INFO("h reads " << reads_after_first << " -> " << clock_reads.load());
    CHECK(clock_reads.load() == reads_after_first);
    driver->set_connected(false);
}

TEST_CASE("Celestron spec audit - PulseGuide rejects a negative duration", "[celestron][telescope][unit][spec-audit]") {
    alpacacore::test::FakeMountServer server([](const std::string& command) {
        if (const auto idle = pulse_mount_idle_reply(command)) return *idle;
        return std::string("00000000,00000000#");
    });
    REQUIRE(server.ok());
    auto driver = connect_pulse_guide_mount(server);
    require_alpaca_error([&] { driver->pulse_guide(0, -5); }, alpacacore::AlpacaError::InvalidValue);
    driver->set_connected(false);
}

TEST_CASE("Celestron spec audit - DoesRefraction is not implemented", "[celestron][telescope][unit][spec-audit]") {
    alpacacore::test::FakeMountServer server([](const std::string& command) {
        if (const auto idle = pulse_mount_idle_reply(command)) return *idle;
        return std::string("00000000,00000000#");
    });
    REQUIRE(server.ok());
    auto driver = connect_pulse_guide_mount(server);
    // The driver applies no refraction correction: ITelescopeV4 has it raise on read and on write.
    require_alpaca_error([&] { (void)driver->get_does_refraction(); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_does_refraction(true); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_does_refraction(false); }, alpacacore::AlpacaError::PropertyNotImplemented);
    driver->set_connected(false);
}

TEST_CASE("Celestron spec audit - PulseGuide rejects a bad direction or duration before the connection check",
          "[celestron][telescope][unit][spec-audit]") {
    alpacacore::test::FakeMountServer server([](const std::string& command) {
        if (const auto idle = pulse_mount_idle_reply(command)) return *idle;
        return std::string("00000000,00000000#");
    });
    REQUIRE(server.ok());
    auto driver = connect_pulse_guide_mount(server);
    // MC_AUX_GUIDE chains 255 cs commands up to 2550 cs: a longer duration used to be cut to 25.5 s silently.
    require_alpaca_error([&] { driver->pulse_guide(0, 25501); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->pulse_guide(4, 100); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->pulse_guide(-1, 100); }, alpacacore::AlpacaError::InvalidValue);
    driver->set_connected(false);
    // The range check precedes the connected check: a disconnected driver says InvalidValue, not NotConnected.
    require_alpaca_error([&] { driver->pulse_guide(0, -5); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->pulse_guide(0, 25501); }, alpacacore::AlpacaError::InvalidValue);
}

#endif  // _WIN32

#ifndef _WIN32
namespace {

// 24-bit signed fraction of a full rotation, as the GPS module reports a latitude or longitude.
int gps_raw_from_degrees(double degrees) {
    return static_cast<int>(std::lround(degrees / 360.0 * 16777216.0)) & 0xFFFFFF;
}

}  // namespace

// Issue #1277: Action("gps_status") reads the GPS module over the AUX bus (GPS_LINKED 0x37, then GPS_GET_LAT 0x01
// and GPS_GET_LONG 0x02): the module's own reading, not the hand controller's stored site.
TEST_CASE("Celestron snapshot - gps_status reports the GPS module's link and reading",
          "[celestron][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());

    CHECK(driver->get_supported_actions() == std::vector<std::string>{"gps_status"});
    CHECK(driver->can_action("gps_status"));
    CHECK(driver->can_action("GPS_Status"));  // action names are case-insensitive
    CHECK(driver->action("GPS_STATUS", "") == driver->action("gps_status", ""));
    require_alpaca_error([&] { (void)driver->action("other_action", ""); },
                         alpacacore::AlpacaError::ActionNotImplemented);

    SECTION("linked: fix with the module's latitude and longitude") {
        st->gps_linked = true;
        st->gps_lat_raw = gps_raw_from_degrees(-33.856784);
        st->gps_long_raw = gps_raw_from_degrees(151.215297);
        const std::string reply = driver->action("gps_status", "");
        CHECK(reply.rfind("{\"fix\":true,", 0) == 0);
        REQUIRE(alpacacore::test::gps_status_number(reply, "latitude").has_value());
        // One count of the 24-bit fraction is 2.1e-5 degrees.
        CHECK(*alpacacore::test::gps_status_number(reply, "latitude") == Catch::Approx(-33.856784).margin(3e-5));
        CHECK(*alpacacore::test::gps_status_number(reply, "longitude") == Catch::Approx(151.215297).margin(3e-5));
        CHECK(reply.find("\"elevation\":null") != std::string::npos);
    }
    SECTION("not linked: no fix, and no coordinates") {
        st->gps_linked = false;
        st->gps_lat_raw = gps_raw_from_degrees(51.477833);
        st->gps_long_raw = gps_raw_from_degrees(-0.0015);
        CHECK(driver->action("gps_status", "") ==
              "{\"fix\":false,\"latitude\":null,\"longitude\":null,\"elevation\":null}");
    }
    driver->set_connected(false);
    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("gps_status"));
    require_alpaca_error([&] { (void)driver->action("gps_status", ""); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Celestron snapshot - gps_status is not listed once the link is lost", "[celestron][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());
    cel::set_status_poll_paused_for_testing(*driver, true);
    CHECK(driver->can_action("gps_status"));

    st->mute = true;
    for (int i = 0; i < 3; ++i) (void)cel::poll_status_now_for_testing(*driver);
    clock.advance(alpacacore::util::kLinkStalenessBound + std::chrono::seconds(1));
    (void)cel::poll_status_now_for_testing(*driver);
    REQUIRE_FALSE(driver->get_connected());

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("gps_status"));
    require_alpaca_error([&] { (void)driver->action("gps_status", ""); }, alpacacore::AlpacaError::NotConnected);
    st->mute = false;
    driver->set_connected(false);
}

TEST_CASE("Celestron snapshot - gps_status is not listed on a mount without a GPS module",
          "[celestron][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<SnapshotMount>();
    st->gps_absent = true;
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return snapshot_reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = snapshot_driver(server, clock);
    REQUIRE(driver->get_connected());

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("gps_status"));
    require_alpaca_error([&] { (void)driver->action("gps_status", ""); },
                         alpacacore::AlpacaError::ActionNotImplemented);
    driver->set_connected(false);
}
#endif

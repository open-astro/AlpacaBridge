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
#include <cstdio>
#include <ctime>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "sidereal_time_check.h"

#ifndef _WIN32
#include "concurrency_stress.h"
#include "fake_celestron_cgx_mount.h"
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
    try {
        static_cast<void>(nexstar.connect(info));
        FAIL("The NexStar backend must not share the direct CGX serial port");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == alpacacore::AlpacaError::InvalidOperation);
    }
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
        case 'L':
            if (st.l_fail.load()) return "";
            return st.slewing.load() ? "1#" : "0#";
        case 'J':
            return "1#";
        case 'p':
            st.slow_count.fetch_add(1);
            return std::string(1, st.pier.load()) + "#";
        case 't':
            st.slow_count.fetch_add(1);
            return st.tracking.load() ? std::string("\x02#") : std::string("\x00#", 2);
        case 'T':
            st.tracking.store(chunk.size() > 1 && chunk[1] != 0);
            return "#";
        case 'W':
        case 'M':
        case 's':
        case 'S':
            return "#";
        case 'P': {  // AUX passthrough: P len dev op ...
            const unsigned char op = chunk.size() > 3 ? static_cast<unsigned char>(chunk[3]) : 0;
            const unsigned char device = chunk.size() > 2 ? static_cast<unsigned char>(chunk[2]) : 0;
            if (op == 0xFE && device == 0x32) return std::string("\x01\x00#", 3);  // DEC autoguider port probe
            if (op == 0xFE) return std::string("\x00\x00#", 3);                    // absent bus devices
            if (op == 0x26) return "#";                                            // MC_AUX_GUIDE
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

template <typename Fn>
std::chrono::microseconds snapshot_timed(Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
}

bool snapshot_has_entry(const alpacacore::TelescopeDriver& driver, const std::string& name) {
    for (const auto& entry : driver.get_device_state()) {
        if (entry.name == name) return true;
    }
    return false;
}

std::chrono::system_clock::time_point snapshot_time_stamp(const alpacacore::TelescopeDriver& driver) {
    for (const auto& entry : driver.get_device_state()) {
        if (entry.name != "TimeStamp") continue;
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
        const auto& text = std::get<std::string>(entry.value);
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
    FAIL("DeviceState has no TimeStamp");
    return {};
}

}  // namespace

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
        CHECK(snapshot_timed(read) < kSnapshotGetterBudget);
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
    const auto age = std::chrono::system_clock::now() - snapshot_time_stamp(*driver);
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

#endif  // _WIN32

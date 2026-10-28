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

#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/logging.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "getter_stall_probe.h"
#include "sidereal_time_check.h"

#ifndef _WIN32
#include "concurrency_stress.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"
#include "fake_zwo_mount.h"
#endif

#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>
#include <alpacacore/util/error_handling.h>

#include <functional>
#include <limits>

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

TEST_CASE("ZWO Mount Telescope Driver - Defaults", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Telescope);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);

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
    REQUIRE(driver->get_can_pulse_guide());
    REQUIRE(driver->get_can_set_guide_rates());
}

TEST_CASE("ZWO Mount Telescope Driver - Target Validation", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(1, conn);

    require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->set_target_right_ascension(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_right_ascension(24.0); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_target_right_ascension(12.0));

    require_alpaca_error([&]() { driver->set_target_declination(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(90.1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_target_declination(45.0));

    require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ZWO Mount Telescope Driver - Target Coordinate Set Tracking", "[zwo][telescope][unit]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeZwoMount mount(clock);
    REQUIRE(mount.ok());
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = mount.port();

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(4, conn, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true));

    REQUIRE_NOTHROW(driver->set_target_right_ascension(3.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 3.0);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    REQUIRE_NOTHROW(driver->set_target_declination(-20.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), -20.0);
    driver->set_connected(false);
}

TEST_CASE("ZWO Mount Telescope Driver - Disconnected Behavior", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(2, conn);

    REQUIRE(driver->get_connected() == false);
    require_alpaca_error([&]() { (void)driver->get_right_ascension(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_declination(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_altitude(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_azimuth(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_tracking(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_tracking(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->slew_to_target_async(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->park(); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
}

TEST_CASE("ZWO Mount Telescope Driver - Site Elevation Validation", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(3, conn);

    require_alpaca_error([&]() { driver->set_site_elevation(std::numeric_limits<double>::quiet_NaN()); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_elevation(-300.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_elevation(10000.1); }, alpacacore::AlpacaError::InvalidValue);

    REQUIRE_NOTHROW(driver->set_site_elevation(-300.0));
    REQUIRE_NOTHROW(driver->set_site_elevation(10000.0));
    ALPACA_REQUIRE_APPROX(driver->get_site_elevation(), 10000.0);
}

// Falsified by: AlpacaCore/include/alpacacore/util/pointing.h validate_ra, validate_latitude: changing either message
// text fails this case.
TEST_CASE("ZWO Mount Telescope Driver - out of range arguments report the shared message before the connection",
          "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(3, conn);
    const auto message_of = [](const std::function<void()>& call) {
        try {
            call();
        } catch (const alpacacore::AlpacaException& ex) {
            return std::string(ex.what());
        }
        return std::string("no exception");
    };
    CHECK(message_of([&] {
              (void)driver->get_destination_side_of_pier(24.0, 0.0);
          }).find("DestinationSideOfPier: RightAscension must be in range") != std::string::npos);
    CHECK(message_of([&] {
              driver->sync_to_coordinates(12.0, 90.1);
          }).find("SyncToCoordinates: Declination must be in range") != std::string::npos);
    CHECK(message_of([&] { driver->set_site_latitude(90.1); }).find("SiteLatitude must be in range") !=
          std::string::npos);
}

TEST_CASE("ZWO Mount Telescope Driver - Site Latitude/Longitude Validation", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(3, conn);

    require_alpaca_error([&]() { driver->set_site_latitude(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_latitude(90.1); }, alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&]() { driver->set_site_longitude(-180.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_longitude(180.1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ZWO Mount Telescope Driver - Telescope Properties", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn);

    CHECK(driver->get_interface_version() >= 3);
    CHECK(driver->get_slew_settle_time() >= 0);
    require_alpaca_error([&]() { driver->set_slew_settle_time(-1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ZWO Mount Telescope Driver - Axis Rate Ranges", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(5, conn);

    const auto primary_ranges = driver->get_axis_rate_ranges(0);
    REQUIRE(primary_ranges.size() == 1);
    ALPACA_REQUIRE_APPROX(primary_ranges.front().first, 0.0);
    REQUIRE(primary_ranges.front().second > 0.0);

    const auto tertiary_ranges = driver->get_axis_rate_ranges(2);
    REQUIRE(tertiary_ranges.empty());

    // Out-of-range axis raises InvalidValue from AxisRates (#516).
    require_alpaca_error([&]() { (void)driver->get_axis_rate_ranges(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { (void)driver->get_axis_rate_ranges(3); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ZWO Mount Telescope Driver - ConnectionType::Auto and enumeration", "[zwo][telescope][unit]") {
    using alpacacore::vendor::zwo::ConnectionType;
    using alpacacore::vendor::zwo::ZWODeviceInfo;

    // ConnectionType::Auto must be a valid transport value distinct from the
    // explicit serial/network modes.
    ConnectionType auto_type = ConnectionType::Auto;
    CHECK(static_cast<int>(auto_type) != static_cast<int>(ConnectionType::Serial));
    CHECK(static_cast<int>(auto_type) != static_cast<int>(ConnectionType::Network));

    // ZWODeviceInfo defaults: serial type, empty port/host, protocol-default
    // TCP port, empty model/label.
    ZWODeviceInfo info;
    CHECK(info.type == ConnectionType::Serial);
    CHECK(info.port_path.empty());
    CHECK(info.host.empty());
    CHECK(info.tcp_port == 4030);
    CHECK(info.model_name.empty());
    CHECK(info.label.empty());

    // enumerate_zwo_mounts with a short probe timeout must return without
    // throwing or hanging. On a host with no ZWO device attached (CI runners),
    // it returns an empty list; with a ZWO device present it returns at least
    // one entry carrying a non-empty model name (probed via :GVP).
    std::vector<ZWODeviceInfo> devices;
    REQUIRE_NOTHROW(devices = alpacacore::vendor::zwo::enumerate_zwo_mounts(100));
    for (const auto& dev : devices) {
        CHECK_FALSE(dev.model_name.empty());
        REQUIRE((dev.type == ConnectionType::Serial || dev.type == ConnectionType::Network));
        if (dev.type == ConnectionType::Serial) {
            CHECK_FALSE(dev.port_path.empty());
        } else {
            CHECK_FALSE(dev.host.empty());
        }
    }
}

TEST_CASE("ZWO Mount Telescope Driver - Auto connect on missing hardware", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Auto;
    conn.response_timeout_ms = 100;

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(6, conn);
    REQUIRE(driver != nullptr);

    // With no ZWO mount reachable, connect() must fail cleanly (no crash,
    // no hang) rather than throwing or leaving a half-open state.
    try {
        driver->set_connected(true);
        // Some platforms (a host with a real ZWO device) may succeed; that is
        // fine. Only assert that a failure surfaces as NotConnected/DriverException
        // rather than corrupting the driver.
    } catch (const alpacacore::AlpacaException&) {
    }
    // The driver must remain in a well-defined (disconnected) state afterwards.
    CHECK(driver->get_connecting() == false);
}

#ifndef _WIN32

// ── The client-clock disagreement warning (#409) ─────────────────────────────

TEST_CASE("ZWO Telescope Driver - a far-off client UTCDate is logged once per connection on a disciplined host",
          "[zwo][telescope][unit]") {
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
            if (level == alpacacore::logging::LogLevel::Warn && component == "ZWO" &&
                message.find("Client UTCDate disagrees") != std::string::npos) {
                ++warns;
            }
        });

    // :SMTI (set date/time) must be acked with "1" or the wrapper throws
    // before the driver caches the instant; :GAT "1#" reports tracking on,
    // and "0#" is a validly terminated reply for everything else.
    alpacacore::test::FakeMountServer server([](const std::string& chunk) -> std::string {
        if (chunk == ":GVP#") return "AM5#";
        if (chunk == ":GMA#") return "012345ABCDEF#";
        if (chunk.find(":GU#") != std::string::npos) return "NG#";
        if (chunk.find(":GR#") != std::string::npos) return "12:00:00#";
        if (chunk.find(":GD#") != std::string::npos) return "+45*00:00#";
        if (chunk.find(":SMTI") != std::string::npos) {
            return "1";
        }
        if (chunk.find(":GAT") != std::string::npos) {
            return "1#";
        }
        return "0#";
    });
    REQUIRE(server.ok());
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 250;
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn);

    // ZWO's set_utc_date() caches the instant even while disconnected (it
    // has no connection check, unlike the other four); the warning must not
    // fire on that path, since nothing was written to any mount.
    const auto far = std::chrono::system_clock::now() + std::chrono::minutes(30);
    driver->set_utc_date(far);
    CHECK(warns.load() == 0);

    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    driver->set_utc_date(std::chrono::system_clock::now());  // agrees: no line, budget untouched
    CHECK(warns.load() == 0);
    driver->set_utc_date(far);
    CHECK(warns.load() == 1);
    driver->set_utc_date(far + std::chrono::seconds(1));
    CHECK(warns.load() == 1);

    // A no-op reconnect (Connected=true while already connected, the
    // Platform 7 handshake path) must NOT re-arm the line: a client that
    // re-sends both on every poll would otherwise get a line per poll.
    driver->set_connected(true);
    driver->set_utc_date(far);
    CHECK(warns.load() == 1);

    // A reconnect re-arms it.
    driver->set_connected(false);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    driver->set_utc_date(far);
    CHECK(warns.load() == 2);
    driver->set_connected(false);
}

// open-astro#714: pulse_guide() took pulse_mutex_ and then mutex_ to publish
// the queue end together with the RA/Dec offsets, while the already-connected
// Connected=true path took mutex_ and then pulse_mutex_ to clear the queue.
// PHD2 pulsing while a second client sends Connect deadlocked the driver, and
// every later call that takes mutex_ (IsPulseGuiding, RightAscension, the
// poll thread) hung with it. The storm below races the two paths from worker
// threads, then proves the driver still answers a mutex_-taking call from a
// probe thread within 1 s (a flag polled from the main thread; never
// std::async, whose future blocks in its destructor and bounds nothing) and
// that the redundant Connected=true still drops the queued pulses (Rule 3).
TEST_CASE(
    "ZWO Telescope Driver - pulse guiding racing a redundant Connected=true neither deadlocks nor "
    "survives the reconnect",
    "[zwo][telescope][unit][pulseguide]") {
    alpacacore::test::FakeTaskClock clock;
    // :Ggr must answer a non-zero guide rate or pulse_guide() returns before
    // it touches pulse_mutex_ and the inversion is never exercised (the same
    // gap hid it from the [stress] case). :GAT "1#" passes the tracking gate,
    // :SMTI "1#" acks the time sync the reconnect path sends (hash-terminated
    // so the wrapper's idle-break wait does not add 100 ms to every refresh),
    // and "0#" is a validly terminated reply for everything else.
    alpacacore::test::FakeMountServer server([](const std::string& chunk) -> std::string {
        if (chunk == ":GVP#") return "AM5#";
        if (chunk == ":GMA#") return "012345ABCDEF#";
        if (chunk.find(":GU#") != std::string::npos) return "NG#";
        if (chunk.find(":GR#") != std::string::npos) return "12:00:00#";
        if (chunk.find(":GD#") != std::string::npos) return "+45*00:00#";
        if (chunk.find(":SMTI") != std::string::npos) {
            return "1#";
        }
        if (chunk.find(":GAT") != std::string::npos) {
            return "1#";
        }
        if (chunk.find(":Ggr") != std::string::npos) {
            return "0.50#";
        }
        return "0#";
    });
    REQUIRE(server.ok());
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 250;
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    // The race: PHD2-style pulses from two threads against Connected=true
    // refreshes from two more, for 3 s. The pulse loops are deliberately
    // unthrottled: the inversion needs a pulse caller that has taken
    // pulse_mutex_ and is parked on mutex_ at the instant a refresh wins
    // mutex_ and then blocks on pulse_mutex_, and a caller that sleeps
    // between pulses parks on one of the earlier, harmless mutex_ takes in
    // pulse_guide() instead. Each refresh (~370 ms over the fake) is one
    // roll of that dice; 3 s of them (16 across the two threads) made the
    // unfixed driver deadlock on every one of 36 measured runs, where 1.5 s
    // still let 1 in 10 through.
    std::atomic<bool> stop{false};
    std::atomic<int> refreshes{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 2; ++i) {
        workers.emplace_back([&] {
            while (!stop.load()) {
                try {
                    driver->pulse_guide(0, 500);
                } catch (const std::exception&) {
                }
            }
        });
        workers.emplace_back([&] {
            while (!stop.load()) {
                try {
                    driver->set_connected(true);
                } catch (const std::exception&) {
                }
                refreshes.fetch_add(1);
            }
        });
    }
    // Count the rolls instead of timing them: 16 refreshes (two threads, about 3 s of them) is what the unfixed driver
    // needed to deadlock. A driver that deadlocks never reaches the count, so the real-time bound (not a driver timer)
    // ends the wait and the probe below reports it.
    constexpr int kRefreshRolls = 16;
    const auto storm_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (refreshes.load() < kRefreshRolls && std::chrono::steady_clock::now() < storm_deadline) {
        std::this_thread::yield();
    }
    stop.store(true);

    // Bounded liveness probe: a second thread makes one mutex_-taking call
    // (IsPulseGuiding) and one lock-free call (Connected) and raises a flag;
    // the main thread polls the flag for at most 1 s. A deadlocked driver
    // never raises it, and the REQUIRE below reports that instead of the
    // process hanging on the join.
    std::atomic<bool> answered{false};
    std::atomic<bool> probe_connected{false};
    std::thread probe([&] {
        static_cast<void>(driver->get_is_pulse_guiding());
        probe_connected.store(driver->get_connected());
        answered.store(true);
    });
    const auto probe_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!answered.load() && std::chrono::steady_clock::now() < probe_deadline) {
        std::this_thread::yield();
    }
    INFO("driver stopped answering mutex_-taking calls while pulse guiding raced Connected=true");
    REQUIRE(answered.load());
    probe.join();
    CHECK(probe_connected.load());
    for (auto& worker : workers) {
        worker.join();
    }

    // The real-time storm above exercises scheduling, not a driver deadline.
    // Rule 3: the redundant refresh drops queued pulses; expire any dequeued pulse and its hold virtually.
    driver->pulse_guide(0, 500);
    CHECK(driver->get_is_pulse_guiding());
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(1500));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    CHECK_FALSE(driver->get_is_pulse_guiding());
    CHECK(driver->get_connected());

    CHECK(alpacacore::test::settle_connected(*driver, false));
}

TEST_CASE("ZWO Telescope Driver - a same-value Tracking write does not drop the in-flight poll",
          "[zwo][telescope][unit][tracking]") {
    // The poll thread is held inside ":GU#" (its snapshot token already taken) while same-value Tracking writes
    // arrive; the mandatory sample must still publish, so the DeviceState TimeStamp age stays within one poll.
    alpacacore::test::FakeTaskClock clock;
    std::atomic<bool> hold_status{false}, status_entered{false}, release_status{false};
    alpacacore::test::FakeMountServer server([&](const std::string& chunk) -> std::string {
        if (chunk == ":GVP#") return "AM5#";
        if (chunk == ":GMA#") return "012345ABCDEF#";
        if (chunk.find(":GU#") != std::string::npos) {
            if (hold_status.load()) {
                status_entered.store(true);
                while (!release_status.load()) std::this_thread::yield();
            }
            return "nN#";  // lowercase n: not tracking, so Tracking=false is the cached no-op value
        }
        if (chunk.find(":GR#") != std::string::npos) return "12:00:00#";
        if (chunk.find(":GD#") != std::string::npos) return "+45*00:00#";
        if (chunk.find(":Ggr") != std::string::npos) return "0.50#";
        return "0#";
    });
    REQUIRE(server.ok());
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 250;
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope_with_site(0, conn, 30.0, -100.0, {}, false, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    driver->set_tracking(false);  // seeds the tracking cache; the writes below are then no-ops

    auto timestamp_age_ms = [&driver] {
        for (const auto& entry : driver->get_device_state()) {
            if (entry.name != "TimeStamp") continue;
            int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
            REQUIRE(std::sscanf(std::get<std::string>(entry.value).c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &y, &mo, &d, &h,
                                &mi, &s, &ms) == 7);
            std::tm tm{};
            tm.tm_year = y - 1900;
            tm.tm_mon = mo - 1;
            tm.tm_mday = d;
            tm.tm_hour = h;
            tm.tm_min = mi;
            tm.tm_sec = s;
            const auto stamp = std::chrono::system_clock::from_time_t(timegm(&tm)) + std::chrono::milliseconds(ms);
            return std::chrono::duration<double, std::milli>(std::chrono::system_clock::now() - stamp).count();
        }
        FAIL("no TimeStamp in DeviceState");
        return 0.0;
    };

    clock.advance(std::chrono::milliseconds(250));  // one clean poll first, so a frame exists
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    hold_status.store(true);
    for (int period = 0; period < 12; ++period) {
        CAPTURE(period);
        status_entered.store(false);
        release_status.store(false);
        clock.advance(std::chrono::milliseconds(250));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!status_entered.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        REQUIRE(status_entered.load());
        driver->set_tracking(false);  // same value, poll in flight
        release_status.store(true);
        REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));  // poll finished, back in its wait
        // A dropped poll leaves the sample a whole period older each time (250 ms, 500 ms, ...); real time
        // elapsed between the two clock reads is microseconds.
        const double age_ms = timestamp_age_ms();
        INFO("TimeStamp age after the poll: " << age_ms << " ms");
        CHECK(age_ms < 125.0);
    }
    release_status.store(true);

    CHECK(alpacacore::test::settle_connected(*driver, false));
}

TEST_CASE("ZWO Telescope Driver - MoveAxis stop on one axis leaves the other axis moving",
          "[zwo][telescope][unit][moveaxis]") {
    // Thread-safe counters: the fake answers on its own thread.
    struct Counts {
        std::atomic<int> stop_all{0};
        std::atomic<int> stop_ew{0};
        std::atomic<int> stop_ns{0};
        std::atomic<int> tracking_on{0};
        std::atomic<bool> tracking{true};
    };
    for (const int stopped_axis : {0, 1}) {
        alpacacore::test::FakeTaskClock clock;
        auto counts = std::make_shared<Counts>();
        alpacacore::test::FakeMountServer server([counts](const std::string& chunk) -> std::string {
            if (chunk == ":GVP#") return "AM5#";
            if (chunk == ":GMA#") return "012345ABCDEF#";
            if (chunk.find(":GR#") != std::string::npos) return "12:00:00#";
            if (chunk.find(":GD#") != std::string::npos) return "+45*00:00#";
            const auto count = [&chunk](const std::string& command) {
                int n = 0;
                for (auto p = chunk.find(command); p != std::string::npos;
                     p = chunk.find(command, p + command.size())) {
                    ++n;
                }
                return n;
            };
            counts->stop_all += count(":Q#");
            counts->stop_ew += count(":Qe#") + count(":Qw#");
            counts->stop_ns += count(":Qn#") + count(":Qs#");
            counts->tracking_on += count(":Te");
            // Like the mount: a jog drops tracking, ":Te" brings it back.
            if (chunk.find(":Me") != std::string::npos || chunk.find(":Mw") != std::string::npos ||
                chunk.find(":Mn") != std::string::npos || chunk.find(":Ms") != std::string::npos) {
                counts->tracking = false;
            }
            if (chunk.find(":Te") != std::string::npos) {
                counts->tracking = true;
            }
            if (chunk.find(":SMTI") != std::string::npos) {
                return "1";
            }
            if (chunk.find(":GAT") != std::string::npos) {
                return counts->tracking.load() ? "1#" : "0#";
            }
            // :GU carries the "n" (not tracking) flag the cache refresh reads.
            if (chunk.find(":GU") != std::string::npos) {
                return counts->tracking.load() ? "0#" : "n#";
            }
            return "0#";
        });
        REQUIRE(server.ok());
        alpacacore::vendor::zwo::ConnectionInfo conn;
        conn.type = alpacacore::vendor::zwo::ConnectionType::Network;
        conn.host = "127.0.0.1";
        conn.tcp_port = server.port();
        conn.response_timeout_ms = 250;
        auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

        driver->move_axis(0, 1.0);
        driver->move_axis(1, 1.0);
        REQUIRE(driver->get_slewing());
        // Outlast the driver's 2 s fast-tracking cache so the restore asks the
        // mount (tracking now off) instead of returning early.
        REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
        clock.advance(std::chrono::milliseconds(2500));
        REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));

        const int tracking_on_before_stop = counts->tracking_on.load();
        driver->move_axis(stopped_axis, 0.0);
        REQUIRE_NOTHROW(driver->command_string(":GVP", false));  // Drain the preceding blind commands.
        CHECK(counts->stop_all.load() == 0);
        CHECK((stopped_axis == 0 ? counts->stop_ew : counts->stop_ns).load() > 0);
        CHECK((stopped_axis == 0 ? counts->stop_ns : counts->stop_ew).load() == 0);
        CHECK(driver->get_slewing());
        // The other axis still jogs: the saved tracking state is handed to it,
        // not restored yet.
        CHECK(counts->tracking_on.load() == tracking_on_before_stop);

        driver->move_axis(1 - stopped_axis, 0.0);
        REQUIRE_NOTHROW(driver->command_string(":GVP", false));
        CHECK(counts->tracking_on.load() > tracking_on_before_stop);
        // The other axis is idle now, so the generic stop is sent.
        CHECK(counts->stop_all.load() > 0);
        CHECK(alpacacore::test::settle_connected(*driver, false));
    }
}

TEST_CASE("ZWO mount site longitude converts between east-positive API and west-positive wire",
          "[zwo][telescope][unit][longitude]") {
    struct SiteCase {
        double longitude;
        const char* wire_longitude;
    };
    for (const auto& site :
         {SiteCase{-82.755, "+082*45:18"}, SiteCase{82.755, "-082*45:18"}, SiteCase{0.0, "+000*00:00"},
          SiteCase{-180.0, "+180*00:00"}, SiteCase{180.0, "-180*00:00"}}) {
        CAPTURE(site.longitude);
        const std::string wire_site = std::string("+28*04:58&") + site.wire_longitude;
        // Fixed wire expectations, independent of the wrapper's read/write conversion.
        alpacacore::test::FakeMountServer server(
            [wire_site, pending = std::string{}](const std::string& chunk) mutable -> std::string {
                pending += chunk;
                std::string response;
                for (auto end = pending.find('#'); end != std::string::npos; end = pending.find('#')) {
                    const auto command = pending.substr(0, end + 1);
                    pending.erase(0, end + 1);
                    if (command == ":GMGE#") {
                        response += wire_site + "#";
                    } else if (command == ":SMGE" + wire_site + "#") {
                        response += "1#";
                    } else {
                        response += "0#";
                    }
                }
                return response;
            });
        REQUIRE(server.ok());
        alpacacore::vendor::zwo::ConnectionInfo conn;
        conn.type = alpacacore::vendor::zwo::ConnectionType::Network;
        conn.host = "127.0.0.1";
        conn.tcp_port = server.port();
        conn.response_timeout_ms = 250;
        alpacacore::vendor::zwo::ZWOMountProtocolWrapper protocol;
        REQUIRE(protocol.connect(conn));
        const auto actual = protocol.get_site_info();
        const double latitude = 28.0 + 4.0 / 60.0 + 58.0 / 3600.0;
        CHECK(std::abs(actual.latitude_degrees - latitude) < 1e-9);
        CHECK(std::abs(actual.longitude_degrees - site.longitude) < 1e-9);
        CHECK_NOTHROW(protocol.set_site_info({latitude, site.longitude}));
    }
}

#endif  // _WIN32

#ifndef _WIN32
// #1298: the LST helper cut UTC to whole seconds.
TEST_CASE("ZWO Mount Telescope Driver - SiderealTime follows the clock to under a second", "[zwo][telescope][unit]") {
    alpacacore::test::FakeMountServer server([](const std::string& chunk) -> std::string {
        if (chunk == ":GVP#") return "AM5#";
        if (chunk == ":GMA#") return "012345ABCDEF#";
        if (chunk.find(":GU#") != std::string::npos) return "NG#";
        if (chunk.find(":GR#") != std::string::npos) return "12:00:00#";
        if (chunk.find(":GD#") != std::string::npos) return "+45*00:00#";
        if (chunk.find(":GMGE#") != std::string::npos) return "+30*00:00&+100*00:00#";
        if (chunk.find(":GAT") != std::string::npos) {
            return "1#";
        }
        return "0#";
    });
    REQUIRE(server.ok());
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 250;
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    alpacacore::test::check_sidereal_time_follows_the_clock(*driver);
    driver->set_connected(false);
}
#endif

#ifndef _WIN32
namespace {

// The AM5 for the getter-stall cases: a FakeZwoMount on a FakeTaskClock. A case holds one exchange open with
// hold_reply() (released when its assertions are done) or delays it with delay_next() (transport latency, real time;
// never a driver timer), and waits for the exchange to be in flight by its command count.
alpacacore::vendor::zwo::ConnectionInfo slow_mount_connection(const alpacacore::test::FakeZwoMount& mount) {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = mount.port();
    conn.response_timeout_ms = 2000;
    return conn;
}

}  // namespace

// Decision 0016: Name and UniqueID need no device access, so a connect that is mid-handshake must not park them.
// Falsified by: zwo_telescope_driver.cpp set_connected(true) holding mutex_ across protocol.get_mount_info()
// (get_name() then waits for the held reply).
TEST_CASE("ZWO Mount Telescope Driver - Name and UniqueID answer while a connect is mid-handshake",
          "[zwo][telescope][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeTaskClock clock;
    FakeZwoMount mount(clock, "AM5");
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, slow_mount_connection(mount), clock);

    mount.hold_reply(":GVP");
    driver->connect();
    REQUIRE(mount.wait_until_held(std::chrono::seconds(2)));

    CHECK(time_call([&] { (void)driver->get_name(); }) < kGetterBudget);
    CHECK(time_call([&] { (void)driver->get_unique_id(); }) < kGetterBudget);
    CHECK(driver->get_connecting());  // the reply is still held, so the timings above are not vacuous

    mount.release();
    REQUIRE(wait_until_connected(*driver));
    CHECK(driver->get_name().rfind("ZWO AM5", 0) == 0);
    CHECK(driver->get_unique_id().rfind("ZWO_MOUNT_AM5", 0) == 0);
    driver->set_connected(false);
}

// A getter that is not connected yet must not run a live exchange in the middle of the handshake.
// Falsified by: zwo_telescope_driver.cpp check_connected() reading connected_ alone (RightAscension then passes
// the check mid-handshake and issues its own serial exchange before Connected reports true).
TEST_CASE("ZWO Mount Telescope Driver - A getter mid-handshake reports NotConnected like Connected does",
          "[zwo][telescope][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeTaskClock clock;
    FakeZwoMount mount(clock, "AM5");
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, slow_mount_connection(mount), clock);

    mount.hold_reply(":GVP");
    driver->connect();
    REQUIRE(mount.wait_until_held(std::chrono::seconds(2)));
    CHECK_FALSE(driver->get_connected());
    require_alpaca_error([&]() { (void)driver->get_declination(); }, alpacacore::AlpacaError::NotConnected);

    mount.release();
    REQUIRE(wait_until_connected(*driver));
    driver->set_connected(false);
}

// Falsified by: zwo_telescope_driver.cpp set_site_latitude()/set_site_longitude() holding mutex_ across
// protocol.set_site_info() (get_name() then waits for the held reply).
TEST_CASE("ZWO Mount Telescope Driver - Name answers while a site write is in flight",
          "[zwo][telescope][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeTaskClock clock;
    FakeZwoMount mount(clock, "AM5");
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, slow_mount_connection(mount), clock);
    driver->connect();
    REQUIRE(wait_until_connected(*driver));

    mount.hold_reply(":SMGE");
    std::thread writer([&] {
        try {
            driver->set_site_longitude(151.215297);
            driver->set_site_latitude(-33.856784);
        } catch (const std::exception&) {
        }
    });
    REQUIRE(mount.wait_until_held(std::chrono::seconds(2)));

    CHECK(time_call([&] { (void)driver->get_name(); }) < kGetterBudget);
    CHECK(time_call([&] { (void)driver->get_unique_id(); }) < kGetterBudget);
    CHECK(time_call([&] { (void)driver->get_aperture_diameter(); }) < kGetterBudget);

    mount.release();
    writer.join();
    driver->set_connected(false);
}

// A site write that lands while the connect refresh is reading the site back must survive that read-back.
// Falsified by: zwo_telescope_driver.cpp apply_or_load_site_info() running without site_write_mutex_ (the
// :GMGE reply then overwrites the latitude the setter just published, and SiteLatitude reads -33.9).
// MUTATION: not claimed for dropping only the lock in sync_mount_time_if_configured(): whether its re-send lands
// after the setter's write is a race, so no test here kills that mutant reliably.
TEST_CASE("ZWO Mount Telescope Driver - A site write during the connect site read-back is kept",
          "[zwo][telescope][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeTaskClock clock;
    FakeZwoMount mount(clock, "AM5");
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, slow_mount_connection(mount), clock);
    // Unreadable until armed, so the connect leaves the site unset and the refresh must read it back.
    mount.override_reply(":GMGE", "0");
    driver->connect();
    REQUIRE(wait_until_connected(*driver));
    driver->set_site_longitude(151.215297);

    // Armed: a stale site (-33.856784 / 151.215297 as the mount's degrees*minutes:seconds pair) that arrives kHoldOpen
    // late, so the setter below lands while the read-back is in flight.
    mount.override_reply(":GMGE", "-33*51:24&+151*12:55");
    mount.delay_next(":GMGE", kHoldOpen);
    const int reads_before = mount.count(":GMGE");
    std::thread refresh([&] {
        try {
            driver->set_connected(true);  // already connected: the refresh path reads the site back
        } catch (const std::exception&) {
        }
    });
    REQUIRE(mount.wait_for_count(":GMGE", reads_before + 1, std::chrono::seconds(2)));
    std::thread writer([&] {
        try {
            driver->set_site_latitude(51.477833);
        } catch (const std::exception&) {
        }
    });
    refresh.join();
    writer.join();
    mount.override_reply(":GMGE", "", 0);

    CHECK(driver->get_site_latitude() == 51.477833);
    // The mount keeps the write too: the last site payload on the wire is the Greenwich latitude, not a stale re-send.
    std::string last_site_write;
    for (const auto& command : mount.commands()) {
        if (command.starts_with(":SMGE")) last_site_write = command;
    }
    CHECK(last_site_write.find("+51*28:40") != std::string::npos);
    driver->set_connected(false);
}

// A GuideRate write that lands while the connect retries its unreadable guide-rate read (150 ms pause, no lock held)
// is newer than the 1.00x the connect then assumes.
// Falsified by: zwo_telescope_driver.cpp set_connected(true) publishing guide_rate_ without the guide_rate_writes_
// check (the connect stores the assumed 1.00x over the 0.30x the client wrote during the retry pause).
TEST_CASE("ZWO Mount Telescope Driver - A guide rate write during the connect read-back is kept",
          "[zwo][telescope][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeTaskClock clock;
    FakeZwoMount mount(clock, "AM5");
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, slow_mount_connection(mount), clock);
    mount.override_reply(":Ggr", "x");  // a non-number, so the connect falls back to 1.00x after its retry
    driver->connect();
    REQUIRE(mount.wait_for_count(":Ggr", 1, std::chrono::seconds(2)));
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the connect is in its 150 ms retry pause
    std::thread writer([&] {
        try {
            const double deg_per_sec = 0.30 * 360.0 / 86164.0905;
            driver->set_guide_rate({deg_per_sec, deg_per_sec});
        } catch (const std::exception&) {
        }
    });
    writer.join();
    clock.advance(std::chrono::milliseconds(150));  // the pause ends; the retry is unreadable too
    REQUIRE(wait_until_connected(*driver));
    mount.override_reply(":Ggr", "", 0);

    const double sidereal_deg_per_sec = 360.0 / 86164.0905;
    CHECK(driver->get_guide_rate().ra / sidereal_deg_per_sec == Catch::Approx(0.30).margin(0.02));
    driver->set_connected(false);
}

// Falsified by: zwo_telescope_driver.cpp set_connected(false) joining disconnect_thread_ inline (the teardown waits
// for the wire behind the exchange in flight, so the call no longer returns at once).
TEST_CASE("ZWO Mount Telescope Driver - A disconnect returns while an exchange is in flight",
          "[zwo][telescope][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeTaskClock clock;
    FakeZwoMount mount(clock, "AM5");
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, slow_mount_connection(mount), clock);
    driver->connect();
    REQUIRE(wait_until_connected(*driver));

    mount.hold_reply(":GVP");
    std::thread worker([&] {
        try {
            (void)driver->command_string(":GVP", false);
        } catch (const std::exception&) {
        }
    });
    REQUIRE(mount.wait_until_held(std::chrono::seconds(2)));

    CHECK(time_call([&] { driver->set_connected(false); }) < kHoldOpen / 3);  // well under the hold, inside 5 s
    mount.release();
    worker.join();
    CHECK_FALSE(driver->get_connected());
}

#endif

// ── #1478 spec-audit items ───────────────────────────────────────────────────

TEST_CASE("ZWO Mount Telescope Driver spec audit - DoesRefraction is not implemented",
          "[zwo][telescope][unit][spec-audit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn);
    // The driver applies no refraction correction: ITelescopeV4 has it raise on read and on write.
    require_alpaca_error([&] { (void)driver->get_does_refraction(); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_does_refraction(true); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_does_refraction(false); }, alpacacore::AlpacaError::PropertyNotImplemented);
}

TEST_CASE("ZWO Mount Telescope Driver spec audit - SiteElevation never set throws InvalidOperation",
          "[zwo][telescope][unit][spec-audit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn);
    // ASCOM time-and-place FAQ (decision 0019): a site value never set reads as InvalidOperation, not 0 m.
    require_alpaca_error([&] { (void)driver->get_site_elevation(); }, alpacacore::AlpacaError::InvalidOperation);
    driver->set_site_elevation(42.0);
    CHECK(driver->get_site_elevation() == 42.0);
}

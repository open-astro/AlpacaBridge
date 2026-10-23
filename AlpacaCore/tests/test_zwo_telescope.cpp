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

#include <algorithm>
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

    require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    require_alpaca_error([&]() { driver->set_target_right_ascension(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_right_ascension(24.0); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_target_right_ascension(12.0));

    require_alpaca_error([&]() { driver->set_target_declination(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(90.1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_target_declination(45.0));

    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 12.0);
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 45.0);
}

TEST_CASE("ZWO Mount Telescope Driver - Target Coordinate Set Tracking", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(4, conn);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(3.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 3.0);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    REQUIRE_NOTHROW(driver->set_target_declination(-20.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), -20.0);
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
        if (chunk.find(":GU#") != std::string::npos) return "NG#";
        if (chunk.find(":GVP") != std::string::npos) return "AM5#";
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
    // :Ggr must answer a non-zero guide rate or pulse_guide() returns before
    // it touches pulse_mutex_ and the inversion is never exercised (the same
    // gap hid it from the [stress] case). :GAT "1#" passes the tracking gate,
    // :SMTI "1#" acks the time sync the reconnect path sends (hash-terminated
    // so the wrapper's idle-break wait does not add 100 ms to every refresh),
    // and "0#" is a validly terminated reply for everything else.
    alpacacore::test::FakeMountServer server([](const std::string& chunk) -> std::string {
        if (chunk.find(":GU#") != std::string::npos) return "NG#";
        if (chunk.find(":GVP") != std::string::npos) return "AM5#";
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
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    // The race: PHD2-style pulses from two threads against Connected=true
    // refreshes from two more, for 3 s. The pulse loops are deliberately
    // unthrottled: the inversion needs a pulse caller that has taken
    // pulse_mutex_ and is parked on mutex_ at the instant a refresh wins
    // mutex_ and then blocks on pulse_mutex_, and a caller that sleeps
    // between pulses parks on one of the earlier, harmless mutex_ takes in
    // pulse_guide() instead. Each refresh (~370 ms over the fake) is one
    // roll of that dice; 3 s of them made the unfixed driver deadlock on
    // every one of 36 measured runs, where 1.5 s still let 1 in 10 through.
    std::atomic<bool> stop{false};
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
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
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
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO("driver stopped answering mutex_-taking calls while pulse guiding raced Connected=true");
    REQUIRE(answered.load());
    probe.join();
    CHECK(probe_connected.load());
    for (auto& worker : workers) {
        worker.join();
    }

    // Rule 3: a pulse queued before a redundant Connected=true does not
    // survive it. Queue one more, refresh, and IsPulseGuiding must drop
    // within one pulse duration plus the 500 ms hold (a task the pulse
    // thread had already dequeued still runs its remaining 500 ms; a
    // QUEUED one is dropped, so nothing follows it).
    driver->pulse_guide(0, 500);
    CHECK(driver->get_is_pulse_guiding());
    driver->set_connected(true);
    const auto clear_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    bool cleared = false;
    while (std::chrono::steady_clock::now() < clear_deadline) {
        if (!driver->get_is_pulse_guiding()) {
            cleared = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(cleared);
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
        if (chunk.find(":GVP") != std::string::npos) return "AM5#";
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
        auto counts = std::make_shared<Counts>();
        alpacacore::test::FakeMountServer server([counts](const std::string& chunk) -> std::string {
            if (chunk.find(":GVP") != std::string::npos) return "AM5#";
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
        auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

        driver->move_axis(0, 1.0);
        driver->move_axis(1, 1.0);
        REQUIRE(driver->get_slewing());
        // Outlast the driver's 2 s fast-tracking cache so the restore asks the
        // mount (tracking now off) instead of returning early.
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));

        const int tracking_on_before_stop = counts->tracking_on.load();
        driver->move_axis(stopped_axis, 0.0);
        // Let the fake see the blind command before counting.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(counts->stop_all.load() == 0);
        CHECK((stopped_axis == 0 ? counts->stop_ew : counts->stop_ns).load() > 0);
        CHECK((stopped_axis == 0 ? counts->stop_ns : counts->stop_ew).load() == 0);
        CHECK(driver->get_slewing());
        // The other axis still jogs: the saved tracking state is handed to it,
        // not restored yet.
        CHECK(counts->tracking_on.load() == tracking_on_before_stop);

        driver->move_axis(1 - stopped_axis, 0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
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
        if (chunk.find(":GU#") != std::string::npos) return "NG#";
        if (chunk.find(":GVP") != std::string::npos) return "AM5#";
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

// A mount that answers ":GVP" (mount info), ":SMGE" (site write) and ":GMGE" (site read) only after kHoldOpen,
// once armed, and reports when such a request is in flight. ":GMGE" is unreadable ("0#") until hold_site_read
// arms it, then reports the Sydney landmark; the last ":SMGE" payload is recorded. ":SMTI" and ":GAT" get "1"
// and "1#". Every other request gets the dumb "0#" reply.
struct SlowMount {
    std::mutex write_mutex;
    std::string last_site_write;  // guarded by write_mutex

    std::string site_write() {
        std::lock_guard<std::mutex> lock(write_mutex);
        return last_site_write;
    }

    std::atomic<bool> hold_info{false};
    std::atomic<bool> hold_site{false};
    std::atomic<bool> hold_site_read{false};
    std::atomic<bool> fail_guide_read{false};  // ":Ggr" replies with a non-number, so the connect falls back to 1.00x
    std::atomic<bool> guide_read_entered{false};
    std::atomic<bool> info_entered{false};
    std::atomic<bool> site_entered{false};
    std::atomic<bool> site_read_entered{false};

    alpacacore::test::FakeMountServer::Responder responder() {
        return [this](const std::string& chunk) -> std::string {
            if (chunk.find(":GU#") != std::string::npos) return "NG#";
            if (chunk.find(":GR#") != std::string::npos) return "12:00:00#";
            if (chunk.find(":GD#") != std::string::npos) return "+45*00:00#";
            if (chunk.find(":GVP") != std::string::npos) {
                if (hold_info.load()) {
                    info_entered = true;
                    std::this_thread::sleep_for(alpacacore::test::kHoldOpen);
                }
                return "AM5#";
            }
            if (chunk.find(":Ggr") != std::string::npos) {
                if (fail_guide_read.load()) {
                    guide_read_entered = true;
                    return "x#";
                }
                return "0.30#";
            }
            if (chunk.find(":GMGE") != std::string::npos) {
                if (hold_site_read.load()) {
                    site_read_entered = true;
                    std::this_thread::sleep_for(alpacacore::test::kHoldOpen);
                }
                // Unreadable until armed, so the connect leaves the site unset and the refresh must read it back.
                // Armed: -33.856784 / 151.215297 as the mount's degrees*minutes:seconds pair.
                return hold_site_read.load() ? "-33*51:24&+151*12:55#" : "0#";
            }
            if (chunk.find(":SMGE") != std::string::npos) {
                {
                    std::lock_guard<std::mutex> lock(write_mutex);
                    last_site_write = chunk;
                }
                if (hold_site.load()) {
                    site_entered = true;
                    std::this_thread::sleep_for(alpacacore::test::kHoldOpen);
                }
                return "1";
            }
            if (chunk.find(":SMTI") != std::string::npos) {
                return "1";
            }
            if (chunk.find(":GAT") != std::string::npos) {
                return "1#";
            }
            return "0#";
        };
    }
};

alpacacore::vendor::zwo::ConnectionInfo slow_mount_connection(const alpacacore::test::FakeMountServer& server) {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 2000;
    return conn;
}

}  // namespace

// Decision 0016: Name and UniqueID need no device access, so a connect that is mid-handshake must not park them.
// Falsified by: zwo_telescope_driver.cpp set_connected(true) holding mutex_ across protocol.get_mount_info()
// (get_name() then waits the whole 300 ms reply).
TEST_CASE("ZWO Mount Telescope Driver - Name and UniqueID answer while a connect is mid-handshake",
          "[zwo][telescope][unit][getter-stall]") {
    using namespace alpacacore::test;
    SlowMount mount;
    mount.hold_info = true;
    FakeMountServer server(mount.responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, slow_mount_connection(server));

    driver->connect();
    REQUIRE(wait_for_flag(mount.info_entered));

    CHECK(time_call([&] { (void)driver->get_name(); }) < kGetterBudget);
    CHECK(time_call([&] { (void)driver->get_unique_id(); }) < kGetterBudget);
    CHECK(driver->get_connecting());  // the reply is still pending, so the timings above are not vacuous

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
    SlowMount mount;
    mount.hold_info = true;
    FakeMountServer server(mount.responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, slow_mount_connection(server));

    driver->connect();
    REQUIRE(wait_for_flag(mount.info_entered));
    CHECK_FALSE(driver->get_connected());
    require_alpaca_error([&]() { (void)driver->get_declination(); }, alpacacore::AlpacaError::NotConnected);

    REQUIRE(wait_until_connected(*driver));
    driver->set_connected(false);
}

// Falsified by: zwo_telescope_driver.cpp set_site_latitude()/set_site_longitude() holding mutex_ across
// protocol.set_site_info() (get_name() then waits the 300 ms reply).
TEST_CASE("ZWO Mount Telescope Driver - Name answers while a site write is in flight",
          "[zwo][telescope][unit][getter-stall]") {
    using namespace alpacacore::test;
    SlowMount mount;
    FakeMountServer server(mount.responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, slow_mount_connection(server));
    driver->connect();
    REQUIRE(wait_until_connected(*driver));

    mount.hold_site = true;
    std::thread writer([&] {
        try {
            driver->set_site_longitude(151.215297);
            driver->set_site_latitude(-33.856784);
        } catch (const std::exception&) {
        }
    });
    REQUIRE(wait_for_flag(mount.site_entered));

    CHECK(time_call([&] { (void)driver->get_name(); }) < kGetterBudget);
    CHECK(time_call([&] { (void)driver->get_unique_id(); }) < kGetterBudget);
    CHECK(time_call([&] { (void)driver->get_aperture_diameter(); }) < kGetterBudget);

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
    SlowMount mount;
    FakeMountServer server(mount.responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, slow_mount_connection(server));
    driver->connect();
    REQUIRE(wait_until_connected(*driver));
    driver->set_site_longitude(151.215297);

    mount.hold_site_read = true;
    std::thread refresh([&] {
        try {
            driver->set_connected(true);  // already connected: the refresh path reads the site back
        } catch (const std::exception&) {
        }
    });
    REQUIRE(wait_for_flag(mount.site_read_entered));
    std::thread writer([&] {
        try {
            driver->set_site_latitude(51.477833);
        } catch (const std::exception&) {
        }
    });
    refresh.join();
    writer.join();
    mount.hold_site_read = false;

    CHECK(driver->get_site_latitude() == 51.477833);
    // The mount keeps the write too: the last site payload on the wire is the Greenwich latitude, not a stale re-send.
    CHECK(mount.site_write().find("+51*28:40") != std::string::npos);
    driver->set_connected(false);
}

// A GuideRate write that lands while the connect retries its unreadable guide-rate read (150 ms pause, no lock held)
// is newer than the 1.00x the connect then assumes.
// Falsified by: zwo_telescope_driver.cpp set_connected(true) publishing guide_rate_ without the guide_rate_writes_
// check (the connect stores the assumed 1.00x over the 0.30x the client wrote during the retry pause).
TEST_CASE("ZWO Mount Telescope Driver - A guide rate write during the connect read-back is kept",
          "[zwo][telescope][unit][getter-stall]") {
    using namespace alpacacore::test;
    SlowMount mount;
    mount.fail_guide_read = true;
    FakeMountServer server(mount.responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, slow_mount_connection(server));
    driver->connect();
    REQUIRE(wait_for_flag(mount.guide_read_entered));
    std::thread writer([&] {
        try {
            const double deg_per_sec = 0.30 * 360.0 / 86164.0905;
            driver->set_guide_rate({deg_per_sec, deg_per_sec});
        } catch (const std::exception&) {
        }
    });
    writer.join();
    REQUIRE(wait_until_connected(*driver));
    mount.fail_guide_read = false;

    const double sidereal_deg_per_sec = 360.0 / 86164.0905;
    CHECK(driver->get_guide_rate().ra / sidereal_deg_per_sec == Catch::Approx(0.30).margin(0.02));
    driver->set_connected(false);
}

// Falsified by: zwo_telescope_driver.cpp set_connected(false) joining disconnect_thread_ inline (the teardown waits
// for the wire behind the 300 ms exchange in flight, so the call no longer returns at once).
TEST_CASE("ZWO Mount Telescope Driver - A disconnect returns while an exchange is in flight",
          "[zwo][telescope][unit][getter-stall]") {
    using namespace alpacacore::test;
    SlowMount mount;
    FakeMountServer server(mount.responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, slow_mount_connection(server));
    driver->connect();
    REQUIRE(wait_until_connected(*driver));

    mount.hold_info = true;
    std::thread worker([&] {
        try {
            (void)driver->command_string(":GVP", false);
        } catch (const std::exception&) {
        }
    });
    REQUIRE(wait_for_flag(mount.info_entered));

    CHECK(time_call([&] { driver->set_connected(false); }) < kHoldOpen / 3);  // well under the hold, inside 5 s
    worker.join();
    CHECK_FALSE(driver->get_connected());
}

namespace {

// A mount whose ":GVP" reply is set per test and which records every request it receives. "silent" sends nothing.
// ":GR" can be made unreadable so the first mandatory telemetry sample fails after a verified identity.
struct IdentityMount {
    std::mutex mutex;
    std::string gvp_reply = "AM5#";
    bool silent = false;
    bool fail_first_sample = false;
    std::vector<std::string> requests;

    void set_reply(std::string reply, bool is_silent = false) {
        std::lock_guard<std::mutex> lock(mutex);
        gvp_reply = std::move(reply);
        silent = is_silent;
    }
    std::vector<std::string> seen() {
        std::lock_guard<std::mutex> lock(mutex);
        return requests;
    }

    alpacacore::test::FakeMountServer::Responder responder() {
        return [this](const std::string& chunk) -> std::string {
            std::lock_guard<std::mutex> lock(mutex);
            requests.push_back(chunk);
            if (chunk.find(":GVP") != std::string::npos) return silent ? std::string() : gvp_reply;
            if (chunk.find(":GU#") != std::string::npos) return "NG#";
            if (chunk.find(":GR#") != std::string::npos) return fail_first_sample ? "x#" : "12:00:00#";
            if (chunk.find(":GD#") != std::string::npos) return fail_first_sample ? "x#" : "+45*00:00#";
            if (chunk.find(":SMTI") != std::string::npos || chunk.find(":SMGE") != std::string::npos) return "1";
            if (chunk.find(":GAT") != std::string::npos) return "1#";
            return "0#";
        };
    }
};

alpacacore::vendor::zwo::ConnectionInfo identity_connection(const alpacacore::test::FakeMountServer& server) {
    auto conn = slow_mount_connection(server);
    conn.response_timeout_ms = 150;
    return conn;
}

bool only_identity_probes(const std::vector<std::string>& requests) {
    return !requests.empty() && std::all_of(requests.begin(), requests.end(),
                                            [](const std::string& r) { return r.find(":GVP") != std::string::npos; });
}

}  // namespace

// Falsified by: zwo_mount_protocol_wrapper.cpp is_zwo_am_model_reply() accepting any non-empty body.
TEST_CASE("ZWO mount identity - only AM models pass the :GVP family check", "[zwo][telescope][unit]") {
    using alpacacore::vendor::zwo::is_zwo_am_model_reply;
    for (const char* ok : {"AM3", "AM5", "AM5N", "AM7", "AM5#", " AM5N \r\n"}) {
        CAPTURE(ok);
        CHECK(is_zwo_am_model_reply(ok));
    }
    for (const char* bad : {"OnStep#", "LX200 GPS#", "0#", "#", "", "AM", "AMx", "AM5n", "AM5NN", "XAM5", "AM5 N"}) {
        CAPTURE(bad);
        CHECK_FALSE(is_zwo_am_model_reply(bad));
    }
}

// Falsified by: zwo_telescope_driver.cpp set_connected(true) catching a failed or rejected get_mount_info() and
// carrying on (Connected publishes true for an OnStep, LX200 GPS, "0" or "" reply, and the site write goes out).
TEST_CASE("ZWO Mount Telescope Driver - A non-ZWO :GVP reply refuses the connect and writes nothing",
          "[zwo][telescope][unit][identity]") {
    using namespace alpacacore::test;
    for (const char* reply : {"OnStep#", "LX200 GPS#", "0#", "#"}) {
        CAPTURE(reply);
        IdentityMount mount;
        mount.set_reply(reply);
        FakeMountServer server(mount.responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, identity_connection(server));
        driver->set_site_latitude(-33.856784);
        driver->set_site_longitude(151.215297);

        require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
        CHECK_FALSE(driver->get_connected());
        CHECK(only_identity_probes(mount.seen()));  // one attempt and one retry, no site/time/rate traffic
        CHECK(mount.seen().size() == 2);
    }
}

// Falsified by: zwo_telescope_driver.cpp set_connected(true) treating a timed-out :GVP as "identity unknown, go on".
TEST_CASE("ZWO Mount Telescope Driver - A silent device refuses the connect with a readable reason",
          "[zwo][telescope][unit][identity]") {
    using namespace alpacacore::test;
    IdentityMount mount;
    mount.set_reply("", /*is_silent=*/true);
    FakeMountServer server(mount.responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, identity_connection(server));

    try {
        driver->set_connected(true);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::NotConnected);
        CHECK(std::string(ex.what()).find("not confirmed as a ZWO AM mount") != std::string::npos);
    }
    CHECK_FALSE(driver->get_connected());
    CHECK(only_identity_probes(mount.seen()));
    CHECK(mount.seen().size() == 2);
}

// Falsified by: zwo_telescope_driver.cpp quoting the device reply into the error without sanitize_for_log().
TEST_CASE("ZWO Mount Telescope Driver - The refusal quotes the reply with control bytes removed",
          "[zwo][telescope][unit][identity]") {
    using namespace alpacacore::test;
    IdentityMount mount;
    mount.set_reply("Evil\x01\x1b[31mX#");
    FakeMountServer server(mount.responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, identity_connection(server));
    try {
        driver->set_connected(true);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        const std::string what = ex.what();
        CHECK(what.find("Evil??[31mX") != std::string::npos);
        CHECK(what.find('\x1b') == std::string::npos);
    }
}

// Falsified by: zwo_telescope_driver.cpp set_connected(true) failing the connect when the first mandatory sample
// fails (decision 0017 keeps unavailable telemetry after a verified identity).
TEST_CASE("ZWO Mount Telescope Driver - A failed first sample after a verified identity still connects",
          "[zwo][telescope][unit][identity]") {
    using namespace alpacacore::test;
    IdentityMount mount;
    mount.fail_first_sample = true;
    FakeMountServer server(mount.responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, identity_connection(server));

    REQUIRE_NOTHROW(driver->set_connected(true));
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

// Falsified by: zwo_telescope_driver.cpp catch (...) cleanup skipping protocol.disconnect() or leaving connected_
// true after a refusal (the retry then reports Connected without re-probing, or the port stays held).
TEST_CASE("ZWO Mount Telescope Driver - A refused connect can be retried against a device that answers AM5",
          "[zwo][telescope][unit][identity]") {
    using namespace alpacacore::test;
    IdentityMount mount;
    mount.set_reply("OnStep#");
    FakeMountServer server(mount.responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, identity_connection(server));

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());

    mount.set_reply("AM5#");
    REQUIRE_NOTHROW(driver->set_connected(true));
    CHECK(driver->get_connected());
    CHECK(driver->get_name().rfind("ZWO AM5", 0) == 0);
    driver->set_connected(false);
}
#endif

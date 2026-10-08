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
#include <exception>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "catch2_compat.h"

#ifndef _WIN32
#include "concurrency_stress.h"
#include "fake_mount_server.h"
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

TEST_CASE("ZWO Telescope Driver - MoveAxis stop on one axis leaves the other axis moving",
          "[zwo][telescope][unit][moveaxis]") {
    // Thread-safe counters: the fake answers on its own thread.
    struct Counts {
        std::atomic<int> stop_all{0};
        std::atomic<int> stop_ew{0};
        std::atomic<int> stop_ns{0};
    };
    for (const int stopped_axis : {0, 1}) {
        auto counts = std::make_shared<Counts>();
        alpacacore::test::FakeMountServer server([counts](const std::string& chunk) -> std::string {
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
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

        driver->move_axis(0, 1.0);
        driver->move_axis(1, 1.0);
        REQUIRE(driver->get_slewing());

        driver->move_axis(stopped_axis, 0.0);
        // Let the fake see the blind command before counting.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(counts->stop_all.load() == 0);
        CHECK((stopped_axis == 0 ? counts->stop_ew : counts->stop_ns).load() > 0);
        CHECK((stopped_axis == 0 ? counts->stop_ns : counts->stop_ew).load() == 0);
        CHECK(driver->get_slewing());

        driver->move_axis(1 - stopped_axis, 0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // The other axis is idle now, so the generic stop is sent.
        CHECK(counts->stop_all.load() > 0);
        CHECK(alpacacore::test::settle_connected(*driver, false));
    }
}

#endif  // _WIN32

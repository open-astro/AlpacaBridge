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

// iOptron getters never wait on device I/O (decision 0016, issues #910 and #1829).
// The mount fake holds its next reply, which keeps a handshake or a cache refill on
// the wire for a known time; a member that needs no device access must answer while
// it is held, and a getter that would ask the mount must not wait for a connect.
// Decision 0005: the budget is wall time spent in a getter, not a driver timeout.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_ioptron_mount.h"
#include "fake_task_clock.h"

namespace {

namespace ioptron = alpacacore::vendor::ioptron;
namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

// One held reply: a connect or a refill waiting on it keeps the wire that long.
constexpr auto kHold = milliseconds(1500);
// Far under one held reply: a getter that waited for it would miss this.
constexpr auto kGetterBudget = milliseconds(100);

ioptron::ConnectionInfo endpoint(int port) {
    ioptron::ConnectionInfo conn;
    conn.type = ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = port;
    return conn;
}

// Times one member. The call may return or throw the contract's AlpacaException (NotConnected during a
// connect, ValueNotSet before a target is written); what is measured is that it answers at once.
template <typename Fn>
void check_answers_at_once(const char* label, Fn&& fn) {
    INFO(label);
    const auto elapsed = alpacacore::test::timed([&] {
        try {
            fn();
        } catch (const AlpacaException&) {
        }
    });
    CHECK(elapsed < kGetterBudget);
}

}  // namespace

TEST_CASE("iOptron telescope - Connected answers at once and reads false while a connect is on the wire",
          "[ioptron][telescope][getter-locks]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;  // no clock-sync tick lands during the case
    auto driver = ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);

    mount.hold_next_reply(kHold);
    const auto start = steady_clock::now();
    driver->connect();
    REQUIRE(mount.wait_until_holding(std::chrono::seconds(5)));

    int samples = 0;
    while (steady_clock::now() - start < milliseconds(1000)) {
        const auto before = steady_clock::now();
        CHECK(driver->get_connecting());
        CHECK_FALSE(driver->get_connected());
        CHECK(steady_clock::now() - before < kGetterBudget);
        ++samples;
        std::this_thread::sleep_for(milliseconds(20));
    }
    CHECK(samples > 0);

    for (int i = 0; i < 200 && driver->get_connecting(); ++i) {
        std::this_thread::sleep_for(milliseconds(50));
    }
    REQUIRE_FALSE(driver->get_connecting());
    CHECK(driver->get_connected());
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}

// Issue #1829: the connect used to hold the driver's state lock across the whole handshake, so every getter
// that takes it waited for the connect. Now the handshake runs with the state lock released: a member that
// needs no device access answers from the driver, and one that would ask the mount answers NotConnected at
// once instead of waiting behind the operation lock the connect holds.
TEST_CASE("iOptron telescope - members that need no device access answer while a connect is on the wire",
          "[ioptron][telescope][getter-locks]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), std::nullopt, std::nullopt,
                                                              120.0, false, clock);

    mount.hold_next_reply(kHold);
    driver->connect();
    REQUIRE(mount.wait_until_holding(std::chrono::seconds(5)));
    REQUIRE(driver->get_connecting());
    const auto sampled_from = steady_clock::now();

    // Served from the driver: identity, limits, capabilities and client-held values.
    check_answers_at_once("Name", [&] { CHECK(driver->get_name() == "iOptron Telescope"); });
    check_answers_at_once("SiteElevation", [&] { CHECK(driver->get_site_elevation() == 120.0); });
    check_answers_at_once("SlewSettleTime", [&] { CHECK(driver->get_slew_settle_time() == 0); });
    check_answers_at_once("CanPark", [&] { CHECK(driver->get_can_park()); });
    check_answers_at_once("AxisRates", [&] { CHECK_FALSE(driver->get_axis_rate_ranges(0).empty()); });
    check_answers_at_once("SupportedActions", [&] { CHECK(driver->get_supported_actions().empty()); });
    // The contract's answers while the connect is still running.
    check_answers_at_once("TargetDeclination", [&] {
        alpacacore::test::require_alpaca_error([&] { driver->get_target_declination(); }, AlpacaError::ValueNotSet);
    });
    check_answers_at_once("TargetRightAscension", [&] {
        alpacacore::test::require_alpaca_error([&] { driver->get_target_right_ascension(); }, AlpacaError::ValueNotSet);
    });
    check_answers_at_once("UTCDate", [&] {
        alpacacore::test::require_alpaca_error([&] { driver->get_utc_date(); }, AlpacaError::NotConnected);
    });
    check_answers_at_once("GuideRate", [&] {
        alpacacore::test::require_alpaca_error([&] { driver->get_guide_rate(); }, AlpacaError::NotConnected);
    });
    check_answers_at_once("TrackingRates", [&] {
        alpacacore::test::require_alpaca_error([&] { driver->get_tracking_rates(); }, AlpacaError::NotConnected);
    });
    // A getter that would ask the mount does not wait for the handshake either.
    check_answers_at_once("Altitude", [&] {
        alpacacore::test::require_alpaca_error([&] { driver->get_altitude(); }, AlpacaError::NotConnected);
    });
    check_answers_at_once("Slewing", [&] {
        alpacacore::test::require_alpaca_error([&] { driver->get_slewing(); }, AlpacaError::NotConnected);
    });
    check_answers_at_once("SideOfPier", [&] {
        alpacacore::test::require_alpaca_error([&] { driver->get_side_of_pier(); }, AlpacaError::NotConnected);
    });
    check_answers_at_once("Tracking", [&] {
        alpacacore::test::require_alpaca_error([&] { driver->get_tracking(); }, AlpacaError::NotConnected);
    });
    // Every sample above landed while the held reply still kept the connect on the wire.
    CHECK(steady_clock::now() - sampled_from < kHold);
    CHECK(driver->get_connecting());

    for (int i = 0; i < 200 && driver->get_connecting(); ++i) {
        std::this_thread::sleep_for(milliseconds(50));
    }
    REQUIRE_FALSE(driver->get_connecting());
    REQUIRE(driver->get_connected());
    CHECK(driver->get_name() == "iOptron HAE29C EQ");
    driver->set_connected(false);
}

// Issue #1829: a getter whose TTL cache has lapsed asks the mount (:GEP# here) and waits for the wire for that
// one read; a member that needs no device access must not wait behind it.
TEST_CASE("iOptron telescope - members that need no device access answer while a cache refill is on the wire",
          "[ioptron][telescope][getter-locks]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), std::nullopt, std::nullopt,
                                                              std::nullopt, false, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // The connect primes every cache for 30 s of task-clock time; past that a position read asks the mount.
    clock.advance(std::chrono::seconds(31));
    mount.hold_next_reply(kHold);
    std::atomic<bool> read_ok{false};
    std::string read_error;
    std::thread reader([&] {
        try {
            static_cast<void>(driver->get_declination());
            read_ok = true;
        } catch (const std::exception& e) {
            read_error = e.what();
        }
    });
    REQUIRE(mount.wait_until_holding(std::chrono::seconds(5)));
    const auto sampled_from = steady_clock::now();

    check_answers_at_once("Name", [&] { CHECK(driver->get_name() == "iOptron HAE29C EQ"); });
    check_answers_at_once("Connected", [&] { CHECK(driver->get_connected()); });
    check_answers_at_once("TargetDeclination", [&] {
        alpacacore::test::require_alpaca_error([&] { driver->get_target_declination(); }, AlpacaError::ValueNotSet);
    });
    check_answers_at_once("GuideRate", [&] { CHECK(driver->get_guide_rate().ra > 0.0); });
    check_answers_at_once("TrackingRates", [&] { CHECK(driver->get_tracking_rates().size() == 5); });
    check_answers_at_once("UTCDate", [&] { static_cast<void>(driver->get_utc_date()); });
    check_answers_at_once("SiteElevation", [&] {
        alpacacore::test::require_alpaca_error([&] { driver->get_site_elevation(); }, AlpacaError::InvalidOperation);
    });
    check_answers_at_once("CanSlewAsync", [&] { CHECK(driver->get_can_slew_async()); });
    check_answers_at_once("IsPulseGuiding", [&] { CHECK_FALSE(driver->get_is_pulse_guiding()); });
    // Every sample above landed while the held reply still kept the refill on the wire.
    CHECK(steady_clock::now() - sampled_from < kHold);

    reader.join();
    INFO(read_error);
    CHECK(read_ok);
    driver->set_connected(false);
}

// Issue #1829, seen on the HAE29C rig: ConformU's first DeviceState lands while the clock-sync body sends the
// site and time commands right after connect, and every getter it reads has a cache the connect primed. A
// cache-backed getter answers from a fresh cache under the state lock alone; it takes the operation lock only to
// refill, so it never waits behind a command on the wire.
TEST_CASE("iOptron telescope - cache-backed getters answer from a fresh cache while a command is on the wire",
          "[ioptron][telescope][getter-locks]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    mount.set_alt_az(45.0, 180.0);  // so the Alt/Az cache is primed at connect like the others
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), std::nullopt, std::nullopt,
                                                              std::nullopt, false, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // The :SRA# of a target write is held: the wrapper waits for its ack, so the command keeps the operation
    // lock for that long (a blind command such as :ST1# only drains an optional ack and would let go early).
    mount.hold_next_reply(kHold);
    std::atomic<bool> write_ok{false};
    std::string write_error;
    std::thread writer([&] {
        try {
            driver->set_target_right_ascension(5.0);
            write_ok = true;
        } catch (const std::exception& e) {
            write_error = e.what();
        }
    });
    REQUIRE(mount.wait_until_holding(std::chrono::seconds(5)));
    const auto sampled_from = steady_clock::now();

    check_answers_at_once("Altitude", [&] { static_cast<void>(driver->get_altitude()); });
    check_answers_at_once("Azimuth", [&] { static_cast<void>(driver->get_azimuth()); });
    check_answers_at_once("RightAscension", [&] { static_cast<void>(driver->get_right_ascension()); });
    check_answers_at_once("Declination", [&] { static_cast<void>(driver->get_declination()); });
    check_answers_at_once("AtHome", [&] { static_cast<void>(driver->get_at_home()); });
    check_answers_at_once("AtPark", [&] { CHECK_FALSE(driver->get_at_park()); });
    check_answers_at_once("Slewing", [&] { CHECK_FALSE(driver->get_slewing()); });
    check_answers_at_once("SideOfPier", [&] { static_cast<void>(driver->get_side_of_pier()); });
    check_answers_at_once("SiteLatitude", [&] { static_cast<void>(driver->get_site_latitude()); });
    check_answers_at_once("SiderealTime", [&] { static_cast<void>(driver->get_sidereal_time()); });
    check_answers_at_once("TrackingRate", [&] { static_cast<void>(driver->get_tracking_rate()); });
    check_answers_at_once("DeviceState", [&] { CHECK_FALSE(driver->get_device_state().empty()); });
    // Every sample above landed while the held reply still kept the write on the wire.
    CHECK(steady_clock::now() - sampled_from < kHold);

    writer.join();
    INFO(write_error);
    CHECK(write_ok);
    driver->set_connected(false);
}

#endif  // _WIN32

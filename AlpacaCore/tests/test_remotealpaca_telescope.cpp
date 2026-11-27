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

// The remotealpaca Telescope proxied device (decision 0023, issue #1753 slice 5, #2710) over the scripted
// FakeHttpClient and a scripted upstream (alpaca_upstream_script.h), with the poll and the MoveAxis cap on the fake
// task clock (decision 0005). Fake evidence only: no socket, no network, no mount. Cases 3 and 4 (and part of 6) of the
// required set come from the driver's contract_sweep.h entry, and the sweep's tier-2 host runs the telescope cases
// (Park and MoveAxis return at once, InvalidValue while parked, the target flags). The rules the Telescope shares with
// the other proxied devices live in UpstreamProxy (upstream_proxy.h) and are covered by
// test_remotealpaca_observingconditions.cpp; the cases here pin what the Telescope's own members do with each of them.
// The [goto-safety] cases are the motion guards that fit a proxy: the MoveAxis cap and the relink stop.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/motion_guards.h>
#include <alpacacore/util/motion_policy.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_telescope_driver.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "alpaca_upstream_script.h"
#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_http_client.h"
#include "fake_task_clock.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
namespace remote = alpacacore::vendor::remotealpaca;
using alpacacore::TelescopeDriver;
using alpacacore::test::alpaca_error_message;
using alpacacore::test::AlpacaUpstreamScript;
using alpacacore::test::error_code_of;
using alpacacore::test::require_alpaca_error;

constexpr std::chrono::milliseconds kRendezvous{5000};
constexpr int kUpstreamPort = 11111;
const std::string kPrefix = "127.0.0.1:11111: ";

// The fake clock, the scripted upstream behind the fake HTTP client, and the driver, destroyed in reverse.
struct Rig {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeHttpClient http;
    AlpacaUpstreamScript upstream{AlpacaUpstreamScript::Kind::Telescope};
    std::unique_ptr<TelescopeDriver> driver;

    explicit Rig(std::chrono::seconds poll = std::chrono::seconds(1), remote::ServerAddressSource self = {},
                 int device_number = 0) {
        upstream.install(http);
        driver =
            remote::create_remotealpaca_telescope(device_number, config(poll), http.client(), std::move(self), clock);
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    ~Rig() { driver.reset(); }

    static remote::RemoteDeviceConfig config(std::chrono::seconds poll) {
        remote::RemoteDeviceConfig config;
        config.host = "127.0.0.1";
        config.port = kUpstreamPort;
        config.upstream_device_number = 0;
        config.poll_period = poll;
        return config;
    }

    // Connect, then wait for the poll to park on the clock.
    void connect() {
        REQUIRE(alpacacore::test::connect_and_park(*driver, clock, kRendezvous));
        REQUIRE(driver->get_connected());
    }

    // One poll period on the clock; returns when the woken poll (and a MoveAxis cap) has parked again or ended.
    void poll(std::chrono::nanoseconds by = std::chrono::seconds(1)) {
        clock.advance(by);
        REQUIRE(clock.wait_for_woken_settled(kRendezvous));
    }

    // Three failed polls in a row latch the fault (decision 0009 rule 1); the upstream answers again after them.
    void fault() {
        http.fail_next(3);
        poll();
        poll();
        poll();
        REQUIRE(driver->get_connected());
        REQUIRE_FALSE(driver->get_link_fault().empty());
    }

    // A refused poll is a lost link at once (decision 0009 point 2(a)).
    void lose() {
        http.refuse_next(1);
        poll();
        REQUIRE_FALSE(driver->get_connected());
    }

    // The PUT members the upstream took from `from` on.
    std::vector<std::string> puts_from(std::size_t from) const {
        const auto all = upstream.puts();
        return {all.begin() + static_cast<std::ptrdiff_t>(std::min(from, all.size())), all.end()};
    }
};

double nan() { return std::numeric_limits<double>::quiet_NaN(); }

// Runs the MoveAxis cap of a move sent now to its 60 s deadline: one poll period at a time, so the frame never ages.
void run_to_cap(Rig& rig) {
    for (int s = 0; s < static_cast<int>(alpacacore::util::kMaxMoveAxisDuration.count()); ++s) rig.poll();
}

// Steps the verified stop's 250 ms polls until Slewing raises (the cap kept its outcome), at most `steps` of them.
int stop_steps_until_raised(Rig& rig, int steps) {
    for (int i = 0; i < steps; ++i) {
        if (error_code_of([&] { (void)rig.driver->get_slewing(); }) != 0) return i;
        rig.poll(alpacacore::util::kStopVerifyPollInterval);
    }
    return -1;
}

}  // namespace

TEST_CASE("Remote Alpaca Telescope Driver - Defaults", "[remotealpaca][telescope][unit]") {
    Rig rig;
    REQUIRE(rig.driver->get_device_type() == alpacacore::DeviceType::Telescope);
    REQUIRE(rig.driver->get_device_number() == 0);
    REQUIRE_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_connecting());
    CHECK(rig.driver->get_name() == "Remote Alpaca Telescope");
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_device_state().empty());
    // The capabilities answer while disconnected: false before any Connect has read the upstream's. CanSlew and
    // CanSlewAltAz are always false: the synchronous slews are not proxied.
    CHECK_FALSE(rig.driver->get_can_park());
    CHECK_FALSE(rig.driver->get_can_slew_async());
    CHECK_FALSE(rig.driver->get_can_pulse_guide());
    CHECK_FALSE(rig.driver->get_can_slew());
    CHECK_FALSE(rig.driver->get_can_slew_alt_az());
    for (int axis = 0; axis < 3; ++axis) CHECK_FALSE(rig.driver->get_can_move_axis(axis));
    require_alpaca_error([&] { (void)rig.driver->get_tracking_rates(); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);  // building the driver touches no network
}

TEST_CASE("Remote Alpaca Telescope Driver - Device metadata", "[remotealpaca][telescope][unit]") {
    Rig rig(std::chrono::seconds(1), {}, 3);
    CHECK(rig.driver->get_device_number() == 3);
    CHECK(rig.driver->get_description() == "Telescope of the upstream Alpaca server 127.0.0.1:11111 device 0");
    CHECK(rig.driver->get_driver_info() == "AlpacaCore Remote Alpaca Telescope Driver");
    CHECK(rig.driver->get_driver_version() == alpacacore::kVersion);
    CHECK(rig.driver->get_interface_version() == 4);
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_TEL_3");
    // Decision 0023 rule 5: the UniqueID is this server's own, never the upstream's, connected or not.
    rig.upstream.set("uniqueid", "\"upstream-unique-id-0000\"");
    rig.connect();
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_TEL_3");
    CHECK(rig.upstream.calls("uniqueid") == 0);
    CHECK(rig.driver->get_name() == "Remote Alpaca Alpaca Simulator Telescope");
    CHECK(rig.driver->get_description() == "ASCOM Alpaca Simulator Telescope");
}

TEST_CASE("Remote Alpaca Telescope Driver - the getters serve the snapshot and the Connect's reads, calling nothing",
          "[remotealpaca][telescope][unit]") {
    Rig rig;
    rig.connect();
    auto& t = *rig.driver;
    const int calls_after_connect = rig.http.calls();
    // The frame.
    CHECK(t.get_right_ascension() == 6.0);
    CHECK(t.get_declination() == 30.0);
    CHECK(t.get_altitude() == 45.0);
    CHECK(t.get_azimuth() == 90.0);
    CHECK(t.get_tracking());
    CHECK_FALSE(t.get_slewing());
    CHECK_FALSE(t.get_at_park());
    CHECK_FALSE(t.get_at_home());
    CHECK(t.get_side_of_pier() == 0);
    CHECK_FALSE(t.get_is_pulse_guiding());
    // What the Connect read.
    CHECK(t.get_can_park());
    CHECK(t.get_can_slew_async());
    CHECK(t.get_can_move_axis(0));
    CHECK(t.get_can_move_axis(1));
    CHECK_FALSE(t.get_can_move_axis(2));
    const auto rates = t.get_axis_rate_ranges(0);
    REQUIRE(rates.size() == 1);
    CHECK(rates[0].first == 0.0);
    CHECK(rates[0].second == 4.0);
    CHECK(t.get_axis_rate_ranges(2).empty());
    CHECK(t.get_alignment_mode() == alpacacore::AlignmentMode::GermanPolar);
    CHECK(t.get_equatorial_system() == alpacacore::EquatorialSystem::Topocentric);
    CHECK(t.get_aperture_diameter() == 0.2);
    CHECK(t.get_focal_length() == 1.2);
    CHECK(t.get_site_latitude() == 51.5);
    CHECK(t.get_site_longitude() == -0.1);
    CHECK(t.get_site_elevation() == 35.0);
    CHECK(t.get_tracking_rates() == std::vector<int>{0, 1, 2, 3});
    CHECK(t.get_tracking_rate() == 0);
    CHECK(t.get_guide_rate().ra == 0.004178);
    CHECK_FALSE(t.get_does_refraction());
    CHECK(t.get_slew_settle_time() == 0);
    CHECK(rig.http.calls() == calls_after_connect);

    // The poll measures the frame only: twelve GETs, none of the Connect's reads.
    const int canpark = rig.upstream.calls("canpark");
    rig.upstream.set("rightascension", "7.25");
    rig.upstream.set("sitelatitude", "10");
    CHECK(t.get_right_ascension() == 6.0);  // until the next poll the frame holds
    const int before = rig.http.calls();
    rig.poll();
    CHECK(rig.http.calls() - before == 12);
    CHECK(t.get_right_ascension() == 7.25);
    CHECK(t.get_site_latitude() == 51.5);
    CHECK(rig.upstream.calls("canpark") == canpark);
}

// The clocks are served advanced by the frame's age; DeviceState's TimeStamp is when the frame was measured.
TEST_CASE("Remote Alpaca Telescope Driver - UTCDate and SiderealTime advance with the frame's age",
          "[remotealpaca][telescope][unit]") {
    Rig rig;
    rig.connect();
    auto& t = *rig.driver;
    const auto upstream_utc = alpacacore::test::parse_timestamp("2026-11-26T12:00:00.000Z");
    CHECK(t.get_utc_date() == upstream_utc);
    CHECK(t.get_sidereal_time() == 12.5);
    remote::set_poll_paused_for_testing(t, true);
    rig.poll(std::chrono::seconds(20));
    CHECK(t.get_utc_date() == upstream_utc + std::chrono::seconds(20));
    CHECK(std::abs(t.get_sidereal_time() - (12.5 + 20.0 * 1.00273790935 / 3600.0)) < 1e-9);
    const auto stamp = alpacacore::test::parse_timestamp(alpacacore::test::time_stamp_of(t));
    const auto age = std::chrono::system_clock::now() - stamp;
    CHECK(age >= std::chrono::seconds(19));
    CHECK(age <= std::chrono::seconds(25));
    // A UTCDate write is forwarded and written through: it reads back at once, still advancing with the age.
    const auto written = alpacacore::test::parse_timestamp("2026-12-01T00:00:00.000Z");
    t.set_utc_date(written);
    CHECK(rig.upstream.calls("utcdate") >= 1);
    CHECK(t.get_utc_date() == written);
    remote::set_poll_paused_for_testing(t, false);
    // An upstream UTCDate that is not ISO 8601 is the upstream's failure, not a lost poll.
    rig.upstream.set("utcdate", "\"yesterday\"");
    rig.poll();
    require_alpaca_error([&] { (void)t.get_utc_date(); }, AlpacaError::DriverException);
    CHECK(t.get_link_fault().empty());
}

TEST_CASE(
    "Remote Alpaca Telescope Driver - SlewToCoordinatesAsync is forwarded and Slewing and the targets read at once",
    "[remotealpaca][telescope][unit]") {
    Rig rig;
    rig.upstream.hold_motion(true);
    rig.connect();
    auto& t = *rig.driver;
    t.slew_to_coordinates_async(10.5, -20.0);
    // Written through: Slewing before any poll, the targets as the upstream set them; RA stays the measured one.
    CHECK(t.get_slewing());
    CHECK(t.get_target_right_ascension() == 10.5);
    CHECK(t.get_target_declination() == -20.0);
    CHECK(t.get_right_ascension() == 6.0);
    rig.poll();
    CHECK(t.get_slewing());  // the upstream still slews
    rig.upstream.finish_motion();
    rig.poll();
    CHECK_FALSE(t.get_slewing());
    CHECK(t.get_right_ascension() == 10.5);
    CHECK(t.get_declination() == -20.0);
    // SlewToTargetAsync and SlewToAltAzAsync: forwarded, Slewing at once.
    t.slew_to_target_async();
    CHECK(t.get_slewing());
    rig.upstream.finish_motion();
    rig.poll();
    t.slew_to_alt_az_async(60.0, 180.0);
    CHECK(t.get_slewing());
    rig.upstream.finish_motion();
    rig.poll();
    CHECK(t.get_altitude() == 60.0);
    CHECK(t.get_azimuth() == 180.0);
    const auto puts = rig.upstream.puts();
    CHECK(std::count(puts.begin(), puts.end(), "slewtocoordinatesasync") == 1);
    CHECK(std::count(puts.begin(), puts.end(), "slewtotargetasync") == 1);
    CHECK(std::count(puts.begin(), puts.end(), "slewtoaltazasync") == 1);
}

// A slew writes Slewing true; only a poll measured after it writes it false. A frame measured before a slew that ended
// at once upstream is no proof either way, so it never reads false before the next poll.
TEST_CASE("Remote Alpaca Telescope Driver - Slewing reads true after a slew until a poll measures the mount",
          "[remotealpaca][telescope][unit]") {
    Rig rig;
    rig.connect();
    auto& t = *rig.driver;
    t.slew_to_coordinates_async(1.0, 2.0);  // the scripted upstream lands at once
    CHECK(t.get_slewing());
    t.park();
    CHECK(t.get_slewing());
    CHECK_FALSE(t.get_at_park());  // AtPark is the poll's
    rig.poll();
    CHECK_FALSE(t.get_slewing());
    CHECK(t.get_at_park());
    // Unpark sets AtPark itself: written through.
    t.unpark();
    CHECK_FALSE(t.get_at_park());
    t.find_home();
    CHECK(t.get_slewing());
    rig.poll();
    CHECK(t.get_at_home());
}

TEST_CASE("Remote Alpaca Telescope Driver - the settings and the targets are forwarded and written through",
          "[remotealpaca][telescope][unit]") {
    Rig rig;
    rig.connect();
    auto& t = *rig.driver;
    // An unset upstream target answers the upstream's own error, unchanged; each setter writes its own only.
    CHECK(alpaca_error_message([&] { (void)t.get_target_right_ascension(); }, AlpacaError::InvalidOperation) ==
          kPrefix + "TargetRightAscension has not been set");
    t.set_target_right_ascension(5.5);
    CHECK(t.get_target_right_ascension() == 5.5);
    require_alpaca_error([&] { (void)t.get_target_declination(); }, AlpacaError::InvalidOperation);
    t.set_target_declination(20.25);
    CHECK(t.get_target_declination() == 20.25);
    t.set_tracking(false);
    CHECK_FALSE(t.get_tracking());
    t.set_tracking_rate(2);
    CHECK(t.get_tracking_rate() == 2);
    t.set_right_ascension_rate(0.5);
    CHECK(t.get_right_ascension_rate() == 0.5);
    t.set_declination_rate(-0.25);
    CHECK(t.get_declination_rate() == -0.25);
    t.set_guide_rate_declination(0.002);
    CHECK(t.get_guide_rate().dec == 0.002);
    CHECK(t.get_guide_rate().ra == 0.004178);  // the other axis is not written back
    t.set_site_latitude(-33.5);
    t.set_site_longitude(151.0);
    t.set_site_elevation(120.0);
    t.set_does_refraction(true);
    t.set_slew_settle_time(3);
    CHECK(t.get_site_latitude() == -33.5);
    CHECK(t.get_site_longitude() == 151.0);
    CHECK(t.get_site_elevation() == 120.0);
    CHECK(t.get_does_refraction());
    CHECK(t.get_slew_settle_time() == 3);
    const auto puts = rig.upstream.puts();
    CHECK(std::count(puts.begin(), puts.end(), "guideraterightascension") == 0);
    // The upstream holds them: the next Connect reads them back.
    t.set_connected(false);
    rig.connect();
    CHECK(t.get_target_right_ascension() == 5.5);
    CHECK(t.get_site_latitude() == -33.5);
    CHECK(t.get_tracking_rate() == 2);
    CHECK_FALSE(t.get_tracking());
    // A write the upstream refuses passes its error through and writes nothing.
    rig.upstream.set_put_error("trackingrate", 0x401, "TrackingRate 3 is not supported");
    CHECK(alpaca_error_message([&] { t.set_tracking_rate(3); }, AlpacaError::InvalidValue) ==
          kPrefix + "TrackingRate 3 is not supported");
    CHECK(t.get_tracking_rate() == 2);
}

TEST_CASE("Remote Alpaca Telescope Driver - PulseGuide, syncs and DestinationSideOfPier are forwarded",
          "[remotealpaca][telescope][unit]") {
    Rig rig;
    rig.connect();
    auto& t = *rig.driver;
    t.pulse_guide(2, 500);
    CHECK(t.get_is_pulse_guiding());  // written through
    rig.poll();
    CHECK(t.get_is_pulse_guiding());  // the upstream still pulses
    rig.upstream.finish_pulse();
    rig.poll();
    CHECK_FALSE(t.get_is_pulse_guiding());
    t.sync_to_coordinates(8.0, 40.0);
    CHECK(t.get_right_ascension() == 8.0);
    CHECK(t.get_declination() == 40.0);
    CHECK(t.get_target_right_ascension() == 8.0);  // read back from the upstream
    t.sync_to_alt_az(30.0, 200.0);
    CHECK(t.get_altitude() == 30.0);
    CHECK(t.get_azimuth() == 200.0);
    t.sync_to_target();
    t.set_park();
    rig.upstream.set("destinationsideofpier", "1");
    CHECK(t.get_destination_side_of_pier(3.0, 10.0) == 1);
    const auto requests = rig.http.requests();
    const bool asked = std::any_of(requests.begin(), requests.end(), [](const auto& r) {
        return r.url.find("/destinationsideofpier?RightAscension=3&Declination=10&") != std::string::npos;
    });
    CHECK(asked);
    const auto puts = rig.upstream.puts();
    CHECK(std::count(puts.begin(), puts.end(), "synctotarget") == 1);
    CHECK(std::count(puts.begin(), puts.end(), "setpark") == 1);
}

TEST_CASE("Remote Alpaca Telescope Driver - Value range validation", "[remotealpaca][telescope][unit]") {
    SECTION("an argument out of every telescope's range is InvalidValue before NotConnected, and sends nothing") {
        Rig rig;
        auto& t = *rig.driver;
        require_alpaca_error([&] { t.slew_to_coordinates_async(24.0, 0.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { t.slew_to_alt_az_async(-91.0, 0.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { t.move_axis(3, 1.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { t.pulse_guide(4, 10); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { t.pulse_guide(0, 60001); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { t.set_side_of_pier(-1); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { t.set_site_elevation(10001.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { t.set_site_latitude(nan()); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { t.set_tracking_rate(-1); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { t.set_guide_rate({-0.1, 0.0}); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { (void)t.get_axis_rate_ranges(-1); }, AlpacaError::InvalidValue);
        rig.connect();
        const int before = rig.http.calls();
        require_alpaca_error([&] { t.move_axis(0, nan()); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { t.sync_to_alt_az(0.0, 361.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { (void)t.get_destination_side_of_pier(0.0, 91.0); }, AlpacaError::InvalidValue);
        CHECK(rig.http.calls() == before);
    }
    SECTION("an argument inside the shared ranges goes to the upstream, and its InvalidValue passes through") {
        Rig rig;
        rig.connect();
        CHECK(alpaca_error_message([&] { rig.driver->move_axis(0, 5.0); }, AlpacaError::InvalidValue) ==
              kPrefix + "MoveAxis rate 5 is too fast");
        CHECK_FALSE(rig.driver->get_slewing());  // a refused move writes nothing through
        CHECK(rig.upstream.calls("moveaxis") == 1);
    }
    SECTION("the factory refuses each config field out of range before any network call") {
        alpacacore::test::FakeTaskClock clock;
        alpacacore::test::FakeHttpClient http;
        for (const auto bad : {std::chrono::seconds(0), std::chrono::seconds(61)}) {
            require_alpaca_error(
                [&] { (void)remote::create_remotealpaca_telescope(0, Rig::config(bad), http.client(), {}, clock); },
                AlpacaError::InvalidValue);
        }
        remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
        config.port = 65536;
        require_alpaca_error([&] { (void)remote::create_remotealpaca_telescope(0, config, http.client(), {}, clock); },
                             AlpacaError::InvalidValue);
        CHECK(http.calls() == 0);
    }
}

TEST_CASE("Remote Alpaca Telescope Driver - AbortSlew and MoveAxis stops are forwarded while faulted, nothing else",
          "[remotealpaca][telescope][unit]") {
    Rig rig;
    rig.upstream.hold_motion(true);
    rig.connect();
    auto& t = *rig.driver;
    t.slew_to_coordinates_async(10.0, 10.0);
    t.move_axis(1, 2.0);
    rig.fault();
    // Faulted: the frame is refused with DriverException, the Connect's reads still answer (static metadata).
    CHECK(alpaca_error_message([&] { (void)t.get_slewing(); }, AlpacaError::DriverException)
              .find("communications compromised") != std::string::npos);
    require_alpaca_error([&] { (void)t.get_right_ascension(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)t.get_tracking(); }, AlpacaError::DriverException);
    CHECK(t.get_can_park());
    CHECK(t.get_site_latitude() == 51.5);
    const std::size_t from = rig.upstream.puts().size();
    // The safety stops go out.
    t.move_axis(1, 0.0);
    t.abort_slew();
    // The commands that need the link are refused and send nothing.
    require_alpaca_error([&] { t.move_axis(0, 1.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { t.slew_to_coordinates_async(1.0, 1.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { t.park(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { t.pulse_guide(0, 100); }, AlpacaError::DriverException);
    require_alpaca_error([&] { t.set_tracking(false); }, AlpacaError::DriverException);
    require_alpaca_error([&] { t.sync_to_coordinates(1.0, 1.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)t.get_destination_side_of_pier(1.0, 1.0); }, AlpacaError::DriverException);
    CHECK(rig.puts_from(from) == std::vector<std::string>{"moveaxis 1 0", "abortslew"});
    // The next good poll clears the fault: the upstream stopped.
    rig.poll();
    CHECK(t.get_link_fault().empty());
    CHECK_FALSE(t.get_slewing());
    CHECK_FALSE(rig.upstream.axis_moving(1));
}

// Commands reach the upstream in caller order (UpstreamProxy's command order mutex): an AbortSlew sent while a slew's
// body is held before its PUT waits for that slew, so the mount never slews after the abort.
TEST_CASE("Remote Alpaca Telescope Driver - an AbortSlew sent during a slew reaches the upstream after the slew",
          "[remotealpaca][telescope][unit]") {
    Rig rig;
    rig.upstream.hold_motion(true);
    rig.connect();
    alpacacore::test::Latch slew_body;
    std::atomic<bool> held{false};  // only the first body (the slew's) holds
    remote::set_command_hook_for_testing(*rig.driver, [&] {
        if (!held.exchange(true)) slew_body.block();
    });
    std::thread slewer([&] { rig.driver->slew_to_coordinates_async(12.0, 45.0); });
    REQUIRE(slew_body.wait_entered());
    // The AbortSlew's caller finds the slew's command running and waits for it: the hook marks the moment.
    alpacacore::test::Latch abort_waits;
    remote::set_command_waiting_hook_for_testing(*rig.driver, [&] { abort_waits.block(); });
    std::thread aborter([&] { rig.driver->abort_slew(); });
    CHECK(abort_waits.wait_entered());  // CHECK, not REQUIRE: the threads are joined below either way
    CHECK(rig.upstream.calls("abortslew") == 0);
    abort_waits.release();
    slew_body.release();
    slewer.join();
    aborter.join();
    const auto puts = rig.upstream.puts();
    REQUIRE(puts.size() >= 2);
    CHECK(puts[puts.size() - 2] == "slewtocoordinatesasync");
    CHECK(puts.back() == "abortslew");
    rig.poll();
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("Remote Alpaca Telescope Driver - Connect and Disconnect are forwarded to the upstream",
          "[remotealpaca][telescope][unit]") {
    SECTION("Platform 7 upstream (ITelescopeV4): Connect and Disconnect") {
        Rig rig;
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 1);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 1);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
    SECTION("an ITelescopeV3 upstream has no Connect: Connected") {
        Rig rig;
        rig.upstream.set("interfaceversion", "3");
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 0);
        CHECK(rig.upstream.upstream_connected());
        CHECK(rig.driver->get_right_ascension() == 6.0);
        rig.driver->set_connected(false);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
    SECTION("an ITelescopeV2 upstream is refused, and left as the Connect found it") {
        Rig rig;
        rig.upstream.set("interfaceversion", "2");
        const std::string text =
            alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
        CHECK(text ==
              "Upstream 127.0.0.1:11111 device 0 is an ITelescopeV2; a Remote Alpaca telescope needs "
              "ITelescopeV3 or later");
        CHECK_FALSE(rig.driver->get_connected());
        CHECK_FALSE(rig.upstream.upstream_connected());
        CHECK(rig.upstream.calls("canpark") == 0);
    }
}

TEST_CASE("Remote Alpaca Telescope Driver - State machine contracts", "[remotealpaca][telescope][unit]") {
    Rig rig;
    auto& t = *rig.driver;
    CHECK_FALSE(t.get_connected());
    CHECK(t.get_device_state().empty());
    require_alpaca_error([&] { (void)t.get_slewing(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)t.get_utc_date(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { t.abort_slew(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { t.move_axis(0, 0.0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { t.park(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)t.get_destination_side_of_pier(1.0, 1.0); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);

    rig.connect();
    CHECK_FALSE(t.get_slewing());  // idle after Connect
    CHECK_FALSE(t.get_is_pulse_guiding());
    const auto state = t.get_device_state();
    REQUIRE(state.size() == 13);  // the twelve ITelescopeV4 members and TimeStamp
    CHECK(state.back().name == "TimeStamp");
    t.set_connected(true);  // idempotent: no second forward
    CHECK(rig.upstream.calls("connect") == 1);

    // A refused command is a lost link at once (decision 0009 point 2(a)): NotConnected, and AbortSlew too.
    rig.http.refuse_next(1);
    require_alpaca_error([&] { t.slew_to_coordinates_async(1.0, 1.0); }, AlpacaError::NotConnected);
    CHECK_FALSE(t.get_connected());
    CHECK(t.get_device_state().empty());
    require_alpaca_error([&] { t.abort_slew(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)t.get_slewing(); }, AlpacaError::NotConnected);
    CHECK(t.get_link_fault().find("connection refused") != std::string::npos);
    // Nothing relinks by itself (decision 0009 rule 4): the poll ended.
    const int calls = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(60));
    CHECK(rig.http.calls() == calls);
    // Only the client's Connect relinks.
    rig.connect();
    CHECK(t.get_link_fault().empty());
    CHECK(t.get_right_ascension() == 6.0);
    t.set_connected(false);
    require_alpaca_error([&] { (void)t.get_right_ascension(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)t.get_site_latitude(); }, AlpacaError::NotConnected);
}

TEST_CASE("Remote Alpaca Telescope Driver - members it does not proxy are not implemented",
          "[remotealpaca][telescope][unit]") {
    Rig rig;
    auto& t = *rig.driver;
    // The synchronous slews (deprecated in ITelescopeV4): the arguments and the connection first, then
    // NotImplemented, never forwarded.
    require_alpaca_error([&] { t.slew_to_coordinates(1.0, 1.0); }, AlpacaError::NotConnected);
    rig.connect();
    require_alpaca_error([&] { t.slew_to_coordinates(25.0, 0.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { t.slew_to_coordinates(1.0, 1.0); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { t.slew_to_target(); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { t.slew_to_alt_az(45.0, 90.0); }, AlpacaError::NotImplemented);
    CHECK_FALSE(t.get_can_slew());
    CHECK_FALSE(t.get_can_slew_alt_az());
    CHECK(rig.upstream.calls("slewtocoordinates") == 0);
    CHECK(rig.upstream.calls("slewtotarget") == 0);
    CHECK(rig.upstream.calls("slewtoaltaz") == 0);
    // The optics are the upstream's: the config-injection setters are not implemented.
    require_alpaca_error([&] { t.set_aperture_diameter(0.3); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { t.set_focal_length(2.0); }, AlpacaError::NotImplemented);
    CHECK(t.get_aperture_diameter() == 0.2);
    // A member the upstream lacks answers its own NotImplemented, unchanged.
    rig.upstream.set_put_error("findhome", 0x400, "FindHome is not implemented");
    CHECK(alpaca_error_message([&] { t.find_home(); }, AlpacaError::NotImplemented) ==
          kPrefix + "FindHome is not implemented");
    // Actions and Command* are not forwarded.
    CHECK(t.get_supported_actions().empty());
    CHECK_FALSE(t.can_action("anything"));
    require_alpaca_error([&] { (void)t.action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)t.command_blind("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)t.command_bool("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)t.command_string("x", false); }, AlpacaError::NotImplemented);
    CHECK(rig.upstream.calls("action") == 0);
}

TEST_CASE("Remote Alpaca Telescope Driver - an upstream refusal passes through unchanged with its host",
          "[remotealpaca][telescope][unit]") {
    Rig rig;
    rig.connect();
    auto& t = *rig.driver;
    t.park();
    rig.poll();
    REQUIRE(t.get_at_park());
    // The upstream judges parked: its InvalidWhileParked passes through, and the link stays up.
    CHECK(alpaca_error_message([&] { t.slew_to_coordinates_async(1.0, 1.0); }, AlpacaError::InvalidWhileParked) ==
          kPrefix + "The mount is parked");
    require_alpaca_error([&] { t.abort_slew(); }, AlpacaError::InvalidWhileParked);
    require_alpaca_error([&] { t.move_axis(0, 1.0); }, AlpacaError::InvalidWhileParked);
    CHECK(t.get_connected());
    CHECK(t.get_link_fault().empty());
    CHECK_FALSE(t.get_slewing());  // a refused slew or MoveAxis wrote nothing through, before any poll
}

TEST_CASE("Remote Alpaca Telescope Driver - Connect refuses an upstream that is this server",
          "[remotealpaca][telescope][unit]") {
    Rig rig(std::chrono::seconds(1), [] { return std::vector<remote::ServerAddress>{{"0.0.0.0", kUpstreamPort}}; });
    const std::string text =
        alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK(text.find("is this server") != std::string::npos);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.http.calls() == 0);
}

TEST_CASE("Remote Alpaca Telescope Driver - the upstream client reads TrackingRates and AxisRates arrays",
          "[remotealpaca][telescope][unit]") {
    alpacacore::test::FakeHttpClient http;
    std::string body;
    http.set_responder([&body](const alpacacore::test::FakeHttpClient::Request&) {
        return alpacacore::util::HttpResponse{200, body};
    });
    remote::RemoteAlpacaClient client(http.client(), {"127.0.0.1", kUpstreamPort, "telescope", 0}, 7);
    body = R"({"Value":[0,2],"ErrorNumber":0,"ErrorMessage":""})";
    CHECK(client.get_int_list("trackingrates") == std::vector<int>{0, 2});
    body = R"({"Value":[{"Maximum":3.5,"Minimum":0.5},{"Minimum":4,"Maximum":4}],"ErrorNumber":0,"ErrorMessage":""})";
    const auto rates = client.get_rate_list("axisrates", {{"Axis", "0"}});
    REQUIRE(rates.size() == 2);
    CHECK(rates[0] == std::pair<double, double>{0.5, 3.5});
    CHECK(rates[1] == std::pair<double, double>{4.0, 4.0});
    // Not an array of the right shape: no usable reply.
    body = R"({"Value":[0,1.5],"ErrorNumber":0,"ErrorMessage":""})";
    require_alpaca_error([&] { (void)client.get_int_list("trackingrates"); }, AlpacaError::DriverException);
    body = R"({"Value":[{"Maximum":3.5}],"ErrorNumber":0,"ErrorMessage":""})";
    require_alpaca_error([&] { (void)client.get_rate_list("axisrates"); }, AlpacaError::DriverException);
}

// ── [goto-safety]: the MoveAxis cap ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Remote Alpaca Telescope MoveAxis - a move not renewed within 60 s is stopped, and Slewing raises it",
          "[remotealpaca][telescope][goto-safety]") {
    Rig rig;
    rig.connect();
    auto& t = *rig.driver;
    t.move_axis(0, 2.0);
    REQUIRE(rig.upstream.axis_moving(0));
    CHECK(t.get_slewing());
    run_to_cap(rig);
    CHECK(stop_steps_until_raised(rig, 40) >= 0);
    CHECK_FALSE(rig.upstream.axis_moving(0));
    CHECK(alpaca_error_message([&] { (void)t.get_slewing(); }, AlpacaError::DriverException) ==
          "MoveAxis ran for 60 s with no new MoveAxis; the axis was stopped");
    CHECK(t.get_connected());
    // The kept outcome ends at the next motion command; the next poll measures the mount still.
    t.move_axis(0, 0.0);
    CHECK(error_code_of([&] { (void)t.get_slewing(); }) == 0);
    rig.poll();
    CHECK_FALSE(t.get_slewing());
}

TEST_CASE("Remote Alpaca Telescope MoveAxis - each new MoveAxis starts the 60 s bound again",
          "[remotealpaca][telescope][goto-safety]") {
    Rig rig;
    rig.connect();
    auto& t = *rig.driver;
    t.move_axis(0, 1.0);
    for (int s = 0; s < 40; ++s) rig.poll();
    t.move_axis(0, 1.5);  // renewed at 40 s: the first bound (60 s) passes with no stop
    for (int s = 0; s < 40; ++s) rig.poll();
    CHECK(rig.upstream.axis_moving(0));
    CHECK(error_code_of([&] { (void)t.get_slewing(); }) == 0);
    const auto puts = rig.upstream.puts();
    CHECK(std::count(puts.begin(), puts.end(), "moveaxis 0 0") == 0);
    // The second bound, 60 s after the renewal, stops it.
    for (int s = 0; s < 20; ++s) rig.poll();
    CHECK(stop_steps_until_raised(rig, 40) >= 0);
    CHECK_FALSE(rig.upstream.axis_moving(0));
}

TEST_CASE("Remote Alpaca Telescope MoveAxis - a stopped move, an AbortSlew or a Disconnect sends no cap stop",
          "[remotealpaca][telescope][goto-safety]") {
    Rig rig;
    rig.connect();
    auto& t = *rig.driver;
    t.move_axis(0, 1.0);
    t.move_axis(0, 0.0);
    t.move_axis(1, 1.0);
    t.abort_slew();
    const std::size_t from = rig.upstream.puts().size();
    run_to_cap(rig);
    for (int i = 0; i < 8; ++i) rig.poll(alpacacore::util::kStopVerifyPollInterval);
    CHECK(rig.puts_from(from).empty());
    CHECK(error_code_of([&] { (void)t.get_slewing(); }) == 0);
    // A Disconnect joins the cap of a move still running.
    t.move_axis(1, 1.0);
    t.set_connected(false);
    rig.connect();
    const std::size_t after = rig.upstream.puts().size();
    run_to_cap(rig);
    CHECK(rig.puts_from(after).empty());
}

// The other axis's move is this server's too: the whole mount's Slewing reads as stopped for the axis being verified.
TEST_CASE("Remote Alpaca Telescope MoveAxis - one axis's cap stops that axis only and verifies while the other moves",
          "[remotealpaca][telescope][goto-safety]") {
    Rig rig;
    rig.connect();
    auto& t = *rig.driver;
    t.move_axis(0, 1.0);
    for (int s = 0; s < 30; ++s) rig.poll();
    t.move_axis(1, 1.0);
    for (int s = 0; s < 30; ++s) rig.poll();
    CHECK(stop_steps_until_raised(rig, 40) >= 0);
    CHECK_FALSE(rig.upstream.axis_moving(0));
    CHECK(rig.upstream.axis_moving(1));
    CHECK(alpaca_error_message([&] { (void)t.get_slewing(); }, AlpacaError::DriverException).find("axis was stopped") !=
          std::string::npos);
}

TEST_CASE(
    "Remote Alpaca Telescope MoveAxis - a cap stop the upstream ignores is sent three times and Slewing raises it",
    "[remotealpaca][telescope][goto-safety]") {
    Rig rig;
    rig.connect();
    auto& t = *rig.driver;
    t.move_axis(0, 1.0);
    // The upstream answers the stop but its mount keeps slewing.
    rig.upstream.set_put_error("moveaxis", 0, "");
    run_to_cap(rig);
    CHECK(stop_steps_until_raised(rig, 40) >= 0);
    const auto puts = rig.upstream.puts();
    CHECK(std::count(puts.begin(), puts.end(), "moveaxis 0 0") == alpacacore::util::kStopResendRounds);
    CHECK(alpaca_error_message([&] { (void)t.get_slewing(); }, AlpacaError::DriverException) ==
          alpacacore::util::kStopNotVerifiedText);
    // A reconnect ends the kept outcome.
    t.set_connected(false);
    rig.connect();
    CHECK(error_code_of([&] { (void)t.get_slewing(); }) == 0);
}

// ── [goto-safety]: the relink stop ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("Remote Alpaca Telescope link loss - a Connect after a loss during a GoTo stops the mount first",
          "[remotealpaca][telescope][goto-safety]") {
    Rig rig;
    rig.upstream.hold_motion(true);
    rig.connect();
    auto& t = *rig.driver;
    t.slew_to_coordinates_async(10.0, 10.0);
    rig.lose();
    // Lost mid-slew: Slewing is unknown (NotConnected), never false.
    require_alpaca_error([&] { (void)t.get_slewing(); }, AlpacaError::NotConnected);
    const std::size_t from = rig.upstream.puts().size();
    rig.connect();
    // The forwarded Connect, then the stops, before any client command; the first frame reads the mount still.
    CHECK(rig.puts_from(from) == std::vector<std::string>{"connect", "abortslew", "moveaxis 0 0", "moveaxis 1 0"});
    CHECK_FALSE(t.get_slewing());
    // Sent once: the next relink owes nothing.
    t.set_connected(false);
    rig.connect();
    CHECK(rig.upstream.calls("abortslew") == 1);
}

TEST_CASE("Remote Alpaca Telescope link loss - a MoveAxis the lost link left running is stopped by the reconnect",
          "[remotealpaca][telescope][goto-safety]") {
    Rig rig;
    rig.connect();
    rig.driver->move_axis(1, 3.0);
    rig.lose();
    rig.connect();
    CHECK(rig.upstream.calls("abortslew") == 1);
    CHECK_FALSE(rig.upstream.axis_moving(1));
}

TEST_CASE("Remote Alpaca Telescope link loss - the stop is kept across the client's disconnect of the lost link",
          "[remotealpaca][telescope][goto-safety]") {
    Rig rig;
    rig.upstream.hold_motion(true);
    rig.connect();
    rig.driver->park();
    rig.lose();
    rig.driver->set_connected(false);  // the lost link is not told (nothing to tell), and the stop stays owed
    rig.connect();
    CHECK(rig.upstream.calls("abortslew") == 1);
}

TEST_CASE("Remote Alpaca Telescope link loss - a connect whose stop gets no reply fails, and the next one sends it",
          "[remotealpaca][telescope][goto-safety]") {
    Rig rig;
    rig.upstream.hold_motion(true);
    rig.connect();
    rig.driver->slew_to_coordinates_async(10.0, 10.0);
    rig.lose();
    // The abortslew PUT gets HTTP 500: no usable reply.
    std::atomic<bool> abort_silent{true};
    rig.http.set_responder([&](const alpacacore::test::FakeHttpClient::Request& request) {
        if (abort_silent.load() && request.method == "PUT" && request.url.find("/abortslew") != std::string::npos) {
            return alpacacore::util::HttpResponse{500, ""};
        }
        return rig.upstream.answer(request);
    });
    require_alpaca_error([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.upstream.calls("abortslew") == 0);  // the HTTP 500 never reached the script
    abort_silent.store(false);
    rig.connect();
    CHECK(rig.upstream.calls("abortslew") == 1);
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("Remote Alpaca Telescope link loss - no stop for a still mount, or for motion this server did not start",
          "[remotealpaca][telescope][goto-safety]") {
    SECTION("this server's slew ended upstream before the relink") {
        Rig rig;
        rig.upstream.hold_motion(true);
        rig.connect();
        rig.driver->slew_to_coordinates_async(10.0, 10.0);
        rig.lose();
        rig.upstream.finish_motion();
        rig.connect();
        CHECK(rig.upstream.calls("abortslew") == 0);
    }
    SECTION("another client's slew: a poll saw this server's own end first") {
        Rig rig;
        rig.connect();
        rig.driver->slew_to_coordinates_async(10.0, 10.0);  // lands at once upstream
        rig.poll();                                         // measured after it: this server's motion ended
        rig.upstream.hold_motion(true);
        {
            // Another client starts a slew upstream.
            alpacacore::test::FakeHttpClient::Request other;
            other.method = "PUT";
            other.url = "http://127.0.0.1:11111/api/v1/telescope/0/slewtocoordinatesasync";
            other.form_body = "RightAscension=3&Declination=3&ClientID=9&ClientTransactionID=1";
            (void)rig.upstream.answer(other);
        }
        rig.lose();
        rig.connect();
        CHECK(rig.upstream.calls("abortslew") == 0);
    }
}

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

// Connect/disconnect/operate concurrency stress for the remotealpaca ObservingConditions, SafetyMonitor, Rotator,
// FilterWheel, Switch, Telescope, Focuser, CoverCalibrator, Dome and Camera proxied devices (issues #101, #1753, #2703
// to #2710). The upstream is the scripted AlpacaUpstreamScript behind FakeHttpClient and the test-only LockedHttpClient
// (decision 0015 rule 2), so every Connect lands and the poll and the command bodies run during the storm. The last
// case of each device races refused-connection losses against the lifecycle and the reads.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/covercalibrator_driver.h>
#include <alpacacore/dome_driver.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/rotator_driver.h>
#include <alpacacore/safetymonitor_driver.h>
#include <alpacacore/switch_driver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_camera_driver.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_covercalibrator_driver.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_dome_driver.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_filterwheel_driver.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_focuser_driver.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_observingconditions_driver.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_rotator_driver.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_safetymonitor_driver.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_switch_driver.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "alpaca_upstream_script.h"
#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "driver_rig.h"
#include "fake_http_client.h"
#include "fake_task_clock.h"
#include "locked_http_client.h"

using alpacacore::AlpacaDriver;

namespace {

namespace remote = alpacacore::vendor::remotealpaca;

// One storm is 750 ms of wall clock. Under ASan + UBSan on arm64 a whole storm can pass without one command reaching a
// connected driver (the nightly runs of 2026-11-24 and 2026-11-27), so a case that checks a command landed storms again
// until `landed()` holds, at most eight times.
void storm_until(AlpacaDriver& driver, const std::function<void(AlpacaDriver&)>& operate,
                 const std::function<bool()>& landed) {
    for (int storm = 0; storm < 8 && !landed(); ++storm) {
        alpacacore::test::run_lifecycle_stress(driver, operate);
    }
}

remote::RemoteObservingConditionsConfig stress_config() {
    remote::RemoteObservingConditionsConfig config;
    config.host = "127.0.0.1";
    config.port = 11111;
    config.poll_period = std::chrono::seconds(1);
    return config;
}

// The fake behind the locked decorator; the fake and the script must outlive the driver.
std::unique_ptr<alpacacore::ObservingConditionsDriver> make_driver(alpacacore::test::FakeHttpClient& http) {
    return remote::create_remotealpaca_observingconditions(
        0, stress_config(), std::make_shared<alpacacore::test::LockedHttpClient>(http.client()), {},
        alpacacore::util::default_task_clock());
}

// `clock` defaults to the real task clock; the refused-connection case passes a fake one it ticks.
std::unique_ptr<alpacacore::SafetyMonitorDriver> make_safetymonitor(
    alpacacore::test::FakeHttpClient& http,
    alpacacore::util::TaskClock& clock = alpacacore::util::default_task_clock()) {
    return remote::create_remotealpaca_safetymonitor(
        0, stress_config(), std::make_shared<alpacacore::test::LockedHttpClient>(http.client()), {}, clock);
}

// Every member the SafetyMonitor serves, each in its own guard.
void operate_safetymonitor(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& sm = static_cast<alpacacore::SafetyMonitorDriver&>(d);
    guard([&] { static_cast<void>(sm.get_is_safe()); });
    guard([&] { static_cast<void>(sm.get_device_state()); });
    guard([&] { static_cast<void>(sm.get_name()); });
    guard([&] { static_cast<void>(sm.get_description()); });
    guard([&] { static_cast<void>(sm.get_link_fault()); });
}

std::unique_ptr<alpacacore::RotatorDriver> make_rotator(alpacacore::test::FakeHttpClient& http) {
    return remote::create_remotealpaca_rotator(0, stress_config(),
                                               std::make_shared<alpacacore::test::LockedHttpClient>(http.client()), {},
                                               alpacacore::util::default_task_clock());
}

// Every member the Rotator serves and its six commands, each in its own guard.
void operate_rotator(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& r = static_cast<alpacacore::RotatorDriver&>(d);
    guard([&] { static_cast<void>(r.get_is_moving()); });
    guard([&] { static_cast<void>(r.get_position()); });
    guard([&] { static_cast<void>(r.get_mechanical_position()); });
    guard([&] { static_cast<void>(r.get_target_position()); });
    guard([&] { static_cast<void>(r.get_reverse()); });
    guard([&] { static_cast<void>(r.get_can_reverse()); });
    guard([&] { static_cast<void>(r.get_step_size()); });
    guard([&] { static_cast<void>(r.get_device_state()); });
    guard([&] { static_cast<void>(r.get_name()); });
    guard([&] { static_cast<void>(r.get_link_fault()); });
    guard([&] { r.move_absolute(200.0); });
    guard([&] { r.move(10.0); });
    guard([&] { r.move_mechanical(50.0); });
    guard([&] { r.sync(100.0); });
    guard([&] { r.set_reverse(false); });
    guard([&] { r.halt(); });
}

std::unique_ptr<alpacacore::FilterWheelDriver> make_filterwheel(alpacacore::test::FakeHttpClient& http) {
    return remote::create_remotealpaca_filterwheel(0, stress_config(),
                                                   std::make_shared<alpacacore::test::LockedHttpClient>(http.client()),
                                                   {}, alpacacore::util::default_task_clock());
}

// Every member the FilterWheel serves and its Position write, each in its own guard. The names write has one entry
// per slot of the scripted wheel, so it is accepted whether the slot count is known or not.
void operate_filterwheel(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& f = static_cast<alpacacore::FilterWheelDriver&>(d);
    guard([&] { static_cast<void>(f.get_position()); });
    guard([&] { static_cast<void>(f.get_names()); });
    guard([&] { static_cast<void>(f.get_focus_offsets()); });
    guard([&] { static_cast<void>(f.get_device_state()); });
    guard([&] { static_cast<void>(f.get_name()); });
    guard([&] { static_cast<void>(f.get_link_fault()); });
    guard([&] { f.set_position(1); });
    guard([&] { f.set_names({"L", "R", "G", "B"}); });
}

std::unique_ptr<alpacacore::SwitchDriver> make_switch(alpacacore::test::FakeHttpClient& http) {
    return remote::create_remotealpaca_switch(0, stress_config(),
                                              std::make_shared<alpacacore::test::LockedHttpClient>(http.client()), {},
                                              alpacacore::util::default_task_clock());
}

// Every member the Switch serves and its writes, each in its own guard; switch 1 of the script is the asynchronous one.
void operate_switch(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& s = static_cast<alpacacore::SwitchDriver&>(d);
    guard([&] { static_cast<void>(s.get_max_switch()); });
    guard([&] { static_cast<void>(s.get_switch(0)); });
    guard([&] { static_cast<void>(s.get_switch_value(1)); });
    guard([&] { static_cast<void>(s.get_state_change_complete(1)); });
    guard([&] { static_cast<void>(s.get_switch_name(1)); });
    guard([&] { static_cast<void>(s.get_switch_description(1)); });
    guard([&] { static_cast<void>(s.get_min_switch_value(1)); });
    guard([&] { static_cast<void>(s.get_max_switch_value(1)); });
    guard([&] { static_cast<void>(s.get_switch_step(1)); });
    guard([&] { static_cast<void>(s.get_can_write(1)); });
    guard([&] { static_cast<void>(s.get_can_async(1)); });
    guard([&] { static_cast<void>(s.get_device_state()); });
    guard([&] { static_cast<void>(s.get_name()); });
    guard([&] { static_cast<void>(s.get_link_fault()); });
    guard([&] { s.set_switch(0, true); });
    guard([&] { s.set_switch_value(1, 50.0); });
    guard([&] { s.set_async_value(1, 60.0); });
    guard([&] { s.set_async(1, false); });
    guard([&] { s.set_switch_name(1, "Heater"); });
}

std::unique_ptr<alpacacore::TelescopeDriver> make_telescope(alpacacore::test::FakeHttpClient& http) {
    return remote::create_remotealpaca_telescope(0, stress_config(),
                                                 std::make_shared<alpacacore::test::LockedHttpClient>(http.client()),
                                                 {}, alpacacore::util::default_task_clock());
}

// The Telescope's frame, Connect reads and capabilities, and its commands (the MoveAxis cap started and cancelled),
// each in its own guard. InvalidWhileParked is the scripted upstream's own answer while a Park has landed.
void operate_telescope(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& t = static_cast<alpacacore::TelescopeDriver&>(d);
    guard([&] { static_cast<void>(t.get_right_ascension()); });
    guard([&] { static_cast<void>(t.get_slewing()); });
    guard([&] { static_cast<void>(t.get_utc_date()); });
    guard([&] { static_cast<void>(t.get_sidereal_time()); });
    guard([&] { static_cast<void>(t.get_at_park()); });
    guard([&] { static_cast<void>(t.get_can_move_axis(0)); });
    guard([&] { static_cast<void>(t.get_can_park()); });
    guard([&] { static_cast<void>(t.get_axis_rate_ranges(0)); });
    guard([&] { static_cast<void>(t.get_site_latitude()); });
    guard([&] { static_cast<void>(t.get_target_right_ascension()); });
    guard([&] { static_cast<void>(t.get_device_state()); });
    guard([&] { static_cast<void>(t.get_link_fault()); });
    guard([&] { t.slew_to_coordinates_async(5.0, 10.0); });
    guard([&] { t.move_axis(0, 1.0); });
    guard([&] { t.move_axis(0, 0.0); });
    guard([&] { t.pulse_guide(0, 10); });
    guard([&] { t.set_target_right_ascension(4.0); });
    guard([&] { t.set_tracking(true); });
    guard([&] { static_cast<void>(t.get_destination_side_of_pier(5.0, 10.0)); });
    guard([&] { t.park(); });
    guard([&] { t.unpark(); });
    guard([&] { t.abort_slew(); });
}

std::unique_ptr<alpacacore::FocuserDriver> make_focuser(alpacacore::test::FakeHttpClient& http) {
    return remote::create_remotealpaca_focuser(0, stress_config(),
                                               std::make_shared<alpacacore::test::LockedHttpClient>(http.client()), {},
                                               alpacacore::util::default_task_clock());
}

// Every member the Focuser serves and its three commands, each in its own guard.
void operate_focuser(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& f = static_cast<alpacacore::FocuserDriver&>(d);
    guard([&] { static_cast<void>(f.get_is_moving()); });
    guard([&] { static_cast<void>(f.get_position()); });
    guard([&] { static_cast<void>(f.get_temperature()); });
    guard([&] { static_cast<void>(f.get_temp_comp()); });
    guard([&] { static_cast<void>(f.get_absolute()); });
    guard([&] { static_cast<void>(f.get_max_step()); });
    guard([&] { static_cast<void>(f.get_max_increment()); });
    guard([&] { static_cast<void>(f.get_step_size()); });
    guard([&] { static_cast<void>(f.get_temp_comp_available()); });
    guard([&] { static_cast<void>(f.get_device_state()); });
    guard([&] { static_cast<void>(f.get_name()); });
    guard([&] { static_cast<void>(f.get_link_fault()); });
    guard([&] { f.move(30000); });
    guard([&] { f.set_temp_comp(false); });
    guard([&] { f.halt(); });
}

std::unique_ptr<alpacacore::CameraDriver> make_camera(alpacacore::test::FakeHttpClient& http) {
    return remote::create_remotealpaca_camera(0, stress_config(),
                                              std::make_shared<alpacacore::test::LockedHttpClient>(http.client()), {},
                                              alpacacore::util::default_task_clock());
}

// The Camera's frame, sensor and settings getters, the image download and its commands, each in its own guard. The
// exposure never finishes on its own (the scripted upstream ends it at StopExposure), so ImageArray is mostly the
// upstream's InvalidOperation and sometimes an image.
void operate_camera(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& c = static_cast<alpacacore::CameraDriver&>(d);
    guard([&] { static_cast<void>(c.get_camera_state()); });
    guard([&] { static_cast<void>(c.get_image_ready()); });
    guard([&] { static_cast<void>(c.get_ccd_temperature()); });
    guard([&] { static_cast<void>(c.get_cooler_power()); });
    guard([&] { static_cast<void>(c.get_percent_completed()); });
    guard([&] { static_cast<void>(c.get_last_exposure_duration()); });
    guard([&] { static_cast<void>(c.get_camera_x_size()); });
    guard([&] { static_cast<void>(c.get_can_abort_exposure()); });
    guard([&] { static_cast<void>(c.get_bin_x()); });
    guard([&] { static_cast<void>(c.get_gain()); });
    guard([&] { static_cast<void>(c.get_device_state()); });
    guard([&] { static_cast<void>(c.get_name()); });
    guard([&] { static_cast<void>(c.get_link_fault()); });
    guard([&] { c.start_exposure(1.0, true); });
    guard([&] { c.stop_exposure(); });
    guard([&] { static_cast<void>(c.get_image_array()); });
    guard([&] { c.set_gain(20); });
    guard([&] { c.set_bin_x(1); });
    guard([&] { c.pulse_guide(0, 10); });
    guard([&] { c.abort_exposure(); });
}

std::unique_ptr<alpacacore::DomeDriver> make_dome(alpacacore::test::FakeHttpClient& http) {
    return remote::create_remotealpaca_dome(0, stress_config(),
                                            std::make_shared<alpacacore::test::LockedHttpClient>(http.client()), {},
                                            alpacacore::util::default_task_clock());
}

// Every member the Dome serves and its ten commands, each in its own guard.
void operate_dome(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& dome = static_cast<alpacacore::DomeDriver&>(d);
    guard([&] { static_cast<void>(dome.get_slewing()); });
    guard([&] { static_cast<void>(dome.get_azimuth()); });
    guard([&] { static_cast<void>(dome.get_altitude()); });
    guard([&] { static_cast<void>(dome.get_at_home()); });
    guard([&] { static_cast<void>(dome.get_at_park()); });
    guard([&] { static_cast<void>(dome.get_shutter_status()); });
    guard([&] { static_cast<void>(dome.get_slaved()); });
    guard([&] { static_cast<void>(dome.get_can_set_azimuth()); });
    guard([&] { static_cast<void>(dome.get_can_slave()); });
    guard([&] { static_cast<void>(dome.get_device_state()); });
    guard([&] { static_cast<void>(dome.get_name()); });
    guard([&] { static_cast<void>(dome.get_link_fault()); });
    guard([&] { dome.slew_to_azimuth(200.0); });
    guard([&] { dome.slew_to_altitude(30.0); });
    guard([&] { dome.sync_to_azimuth(100.0); });
    guard([&] { dome.find_home(); });
    guard([&] { dome.park(); });
    guard([&] { dome.set_park(); });
    guard([&] { dome.open_shutter(); });
    guard([&] { dome.close_shutter(); });
    guard([&] { dome.set_slaved(false); });
    guard([&] { dome.abort_slew(); });
}

std::unique_ptr<alpacacore::CoverCalibratorDriver> make_covercalibrator(alpacacore::test::FakeHttpClient& http) {
    return remote::create_remotealpaca_covercalibrator(
        0, stress_config(), std::make_shared<alpacacore::test::LockedHttpClient>(http.client()), {},
        alpacacore::util::default_task_clock());
}

// Every member the CoverCalibrator serves and its five commands, each in its own guard.
void operate_covercalibrator(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& c = static_cast<alpacacore::CoverCalibratorDriver&>(d);
    guard([&] { static_cast<void>(c.get_cover_state()); });
    guard([&] { static_cast<void>(c.get_cover_moving()); });
    guard([&] { static_cast<void>(c.get_calibrator_state()); });
    guard([&] { static_cast<void>(c.get_calibrator_changing()); });
    guard([&] { static_cast<void>(c.get_brightness()); });
    guard([&] { static_cast<void>(c.get_max_brightness()); });
    guard([&] { static_cast<void>(c.get_device_state()); });
    guard([&] { static_cast<void>(c.get_name()); });
    guard([&] { static_cast<void>(c.get_link_fault()); });
    guard([&] { c.open_cover(); });
    guard([&] { c.close_cover(); });
    guard([&] { c.halt_cover(); });
    guard([&] { c.calibrator_on(50); });
    guard([&] { c.calibrator_off(); });
}

// Every member the driver serves, each in its own guard so one throw skips no later call.
void operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& oc = static_cast<alpacacore::ObservingConditionsDriver&>(d);
    guard([&] { static_cast<void>(oc.get_temperature()); });
    guard([&] { static_cast<void>(oc.get_humidity()); });
    guard([&] { static_cast<void>(oc.get_cloud_cover()); });
    guard([&] { static_cast<void>(oc.get_average_period()); });
    guard([&] { oc.set_average_period(0.0); });
    guard([&] { static_cast<void>(oc.get_time_since_last_update("Temperature")); });
    guard([&] { static_cast<void>(oc.get_sensor_description("Temperature")); });
    guard([&] { static_cast<void>(oc.get_device_state()); });
    guard([&] { static_cast<void>(oc.get_name()); });
    guard([&] { static_cast<void>(oc.get_link_fault()); });
    guard([&] { oc.refresh(); });
}

}  // namespace

TEST_CASE("Remote Alpaca observing conditions - concurrent connect/disconnect/operate stress",
          "[remotealpaca][observingconditions][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream;
    upstream.install(http);
    auto driver = make_driver(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down. InvalidOperation: a command started while a
    // Disconnect joins the command slot is refused ("try again", issue #1713).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate(guard, d); });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca observing conditions - destruction races an in-flight connect",
          "[remotealpaca][observingconditions][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream;
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_driver(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca observing conditions - racing disconnect is never dropped",
          "[remotealpaca][observingconditions][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream;
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_driver(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// Decision 0009 point 2(a): one call in 150 is refused (a Connect makes some sixty, so most land), so the poll and
// Refresh lose the link at once while lifecycle threads connect and disconnect it and readers race the getters. TSan
// checks the loss path's writes (connected_, the kept reason, the dropped frame) against the getters, the commands and
// set_connected().
TEST_CASE("Remote Alpaca observing conditions - refused-connection losses race connects, disconnects and reads",
          "[remotealpaca][observingconditions][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream;
    std::atomic<int> calls{0};
    http.set_responder([&upstream, &calls](const alpacacore::test::FakeHttpClient::Request& request) {
        if (calls.fetch_add(1) % 150 == 149) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_driver(http);

    // A refused command is NotConnected (the loss is its answer); InvalidOperation as above.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    storm_until(
        *driver, [&guard](AlpacaDriver& d) { operate(guard, d); },
        [&upstream] { return upstream.calls("refresh") > 0; });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // the Disconnect ended any kept reason
    CHECK(upstream.calls("refresh") > 0);     // commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca safety monitor - concurrent connect/disconnect/operate stress",
          "[remotealpaca][safetymonitor][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::SafetyMonitor};
    upstream.install(http);
    auto driver = make_safetymonitor(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down; nothing else (no fault, no command).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate_safetymonitor(guard, d); });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca safety monitor - destruction races an in-flight connect",
          "[remotealpaca][safetymonitor][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::SafetyMonitor};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_safetymonitor(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca safety monitor - racing disconnect is never dropped",
          "[remotealpaca][safetymonitor][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::SafetyMonitor};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_safetymonitor(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// One call in 20 is refused, so a poll loses the link at once while lifecycle threads connect and disconnect it and
// readers race IsSafe. The storm is shorter than one real poll period, so the driver polls on a fake task clock that a
// ticker thread advances a period at a time; a poll the ticker outruns lets the frame age past the loss bound, the
// other loss path (the module's, found by the poll or by IsSafe's own read). A lost link reads IsSafe false with no
// exception, so the guard allows NotConnected only, from a never-connected, connecting or client-disconnected driver;
// the scripted upstream always reads safe, so a false read is a lost link's answer, and the case requires one. TSan
// checks the loss path's writes (connected_, the kept reason, the module's loss latch) against IsSafe's lost() read.
TEST_CASE("Remote Alpaca safety monitor - refused-connection losses race connects, disconnects and IsSafe",
          "[remotealpaca][safetymonitor][stress]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::SafetyMonitor};
    std::atomic<int> calls{0};
    http.set_responder([&upstream, &calls](const alpacacore::test::FakeHttpClient::Request& request) {
        if (calls.fetch_add(1) % 20 == 19) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_safetymonitor(http, clock);
    std::atomic<bool> storm{true};
    std::thread ticker([&clock, &storm] {
        while (storm.load()) {
            clock.advance(std::chrono::seconds(1));  // one poll period: every parked poll samples once
            std::this_thread::yield();
        }
    });

    std::atomic<int> unsafe_reads{0};
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    storm_until(
        *driver,
        [&guard, &unsafe_reads](AlpacaDriver& d) {
            auto& sm = static_cast<alpacacore::SafetyMonitorDriver&>(d);
            guard([&] {
                if (!sm.get_is_safe()) unsafe_reads.fetch_add(1);
            });
            operate_safetymonitor(guard, d);
        },
        [&unsafe_reads] { return unsafe_reads.load() > 0; });

    storm.store(false);
    ticker.join();
    // The last connect() may still own its connection task, so a bare set_connected(false) can be recorded against
    // it and return before the task has run (open-astro#326): settle instead, then read the kept reason.
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // the Disconnect ended any kept reason
    CHECK(unsafe_reads.load() > 0);           // the loss path ran during the storm
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca rotator - concurrent connect/disconnect/operate stress", "[remotealpaca][rotator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Rotator};
    upstream.install(http);
    auto driver = make_rotator(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down. InvalidOperation: a command started while a
    // Disconnect joins the command slot is refused ("try again", issue #1713).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    storm_until(
        *driver, [&guard](AlpacaDriver& d) { operate_rotator(guard, d); },
        [&upstream] { return upstream.calls("moveabsolute") > 0; });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(upstream.calls("moveabsolute") > 0);  // the commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca rotator - destruction races an in-flight connect", "[remotealpaca][rotator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Rotator};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_rotator(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca rotator - racing disconnect is never dropped", "[remotealpaca][rotator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Rotator};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_rotator(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// Decision 0009 point 2(a): one call in 40 is refused (a Connect makes some sixteen, so most land), so the poll and
// the commands lose the link at once while lifecycle threads connect and disconnect it and readers race the getters.
// TSan checks the loss path's writes (connected_, the kept reason, the dropped frame) against the getters, the six
// command bodies and their write-throughs, the InterfaceVersion the Connect stores for the poll, and set_connected().
TEST_CASE("Remote Alpaca rotator - refused-connection losses race connects, disconnects, commands and reads",
          "[remotealpaca][rotator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Rotator};
    std::atomic<int> calls{0};
    http.set_responder([&upstream, &calls](const alpacacore::test::FakeHttpClient::Request& request) {
        if (calls.fetch_add(1) % 40 == 39) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_rotator(http);

    // A refused command is NotConnected (the loss is its answer); InvalidOperation as above.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    storm_until(
        *driver, [&guard](AlpacaDriver& d) { operate_rotator(guard, d); },
        [&upstream] { return upstream.calls("halt") > 0; });

    // The last connect() may still own its connection task (open-astro#326): settle instead of a bare Disconnect.
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // the Disconnect ended any kept reason
    CHECK(upstream.calls("halt") > 0);        // commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

// CanReverse answers while disconnected and while connected, so no read of it throws, whatever Disconnect or Connect
// races it: the driver picks the answer under one state-lock hold. InvalidOperation is never CanReverse's answer; it
// only replaces the guard's default set, so a NotConnected is unexpected here.
TEST_CASE("Remote Alpaca rotator - CanReverse never throws while connects and disconnects race it",
          "[remotealpaca][rotator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Rotator};
    upstream.install(http);
    auto driver = make_rotator(http);
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::InvalidOperation};
    std::atomic<int> false_reads{0};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard, &false_reads](AlpacaDriver& d) {
        auto& r = static_cast<alpacacore::RotatorDriver&>(d);
        for (int i = 0; i < 50; ++i) {
            guard([&] {
                if (!r.get_can_reverse()) false_reads.fetch_add(1);
            });
        }
    });
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(false_reads.load() == 0);  // the upstream answers true, and so does a driver never connected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca filter wheel - concurrent connect/disconnect/operate stress",
          "[remotealpaca][filterwheel][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::FilterWheel};
    upstream.install(http);
    auto driver = make_filterwheel(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down. InvalidOperation: a command started while a
    // Disconnect joins the command slot is refused ("try again", issue #1713).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate_filterwheel(guard, d); });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(upstream.last_position_put() == "1");  // the writes ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca filter wheel - destruction races an in-flight connect",
          "[remotealpaca][filterwheel][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::FilterWheel};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_filterwheel(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca filter wheel - racing disconnect is never dropped", "[remotealpaca][filterwheel][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::FilterWheel};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_filterwheel(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// Decision 0009 point 2(a): one call in 40 is refused (a Connect makes some nine, so most land), so the poll and the
// Position writes lose the link at once while lifecycle threads connect and disconnect it and readers race the
// getters. TSan checks the loss path's writes (connected_, the kept reason, the dropped frame) and the Connect's
// slot store against the getters, the names write, the Position bodies and their write-throughs, and set_connected().
TEST_CASE("Remote Alpaca filter wheel - refused-connection losses race connects, disconnects, writes and reads",
          "[remotealpaca][filterwheel][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::FilterWheel};
    std::atomic<int> calls{0};
    http.set_responder([&upstream, &calls](const alpacacore::test::FakeHttpClient::Request& request) {
        if (calls.fetch_add(1) % 40 == 39) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_filterwheel(http);

    // A refused write is NotConnected (the loss is its answer); InvalidOperation as above.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate_filterwheel(guard, d); });

    // The last connect() may still own its connection task (open-astro#326): settle instead of a bare Disconnect.
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());     // the Disconnect ended any kept reason
    CHECK(upstream.last_position_put() == "1");  // writes ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca switch - concurrent connect/disconnect/operate stress", "[remotealpaca][switch][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Switch};
    upstream.install(http);
    auto driver = make_switch(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down. InvalidOperation: a command started while a
    // Disconnect joins the command slot is refused ("try again", issue #1713).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    storm_until(
        *driver, [&guard](AlpacaDriver& d) { operate_switch(guard, d); },
        [&upstream] { return upstream.calls("setswitch") > 0; });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(upstream.calls("setswitch") > 0);  // the writes ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca switch - destruction races an in-flight connect", "[remotealpaca][switch][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Switch};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_switch(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca switch - racing disconnect is never dropped", "[remotealpaca][switch][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Switch};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_switch(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// Decision 0009 point 2(a): one call in 60 is refused (a Connect makes some thirty-five), so the poll and the writes
// lose the link at once while lifecycle threads connect and disconnect it and readers race the getters. TSan checks
// the loss path's writes (connected_, the kept reason, the dropped frame) against the getters, the write bodies and
// their write-throughs (the frame and the SetSwitchName write to the Connect's metadata), and set_connected().
TEST_CASE("Remote Alpaca switch - refused-connection losses race connects, disconnects, writes and reads",
          "[remotealpaca][switch][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Switch};
    std::atomic<int> calls{0};
    http.set_responder([&upstream, &calls](const alpacacore::test::FakeHttpClient::Request& request) {
        if (calls.fetch_add(1) % 60 == 59) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_switch(http);

    // A refused write is NotConnected (the loss is its answer); InvalidOperation as above.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    storm_until(
        *driver, [&guard](AlpacaDriver& d) { operate_switch(guard, d); },
        [&upstream] { return upstream.calls("setswitchvalue") > 0; });

    // The last connect() may still own its connection task (open-astro#326): settle instead of a bare Disconnect.
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());      // the Disconnect ended any kept reason
    CHECK(upstream.calls("setswitchvalue") > 0);  // writes ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca telescope - concurrent connect/disconnect/operate stress",
          "[remotealpaca][telescope][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Telescope};
    upstream.install(http);
    auto driver = make_telescope(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down. InvalidOperation: a command started while a
    // Disconnect joins the command slot is refused ("try again", issue #1713). InvalidWhileParked: the upstream's
    // answer to a command another thread's Park has made a parked mount refuse; the guard's set is one per case, so
    // an unexpected code still fails.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation,
                                            alpacacore::AlpacaError::InvalidWhileParked};
    storm_until(
        *driver, [&guard](AlpacaDriver& d) { operate_telescope(guard, d); },
        [&upstream] { return upstream.calls("abortslew") > 0; });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    // The commands ran on a connected driver. A stop, not a slew: a slew is refused while a Disconnect runs, and the
    // lifecycle threads disconnect often enough that a run can send none.
    CHECK(upstream.calls("abortslew") > 0);
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca telescope - destruction races an in-flight connect", "[remotealpaca][telescope][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Telescope};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_telescope(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca telescope - racing disconnect is never dropped", "[remotealpaca][telescope][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Telescope};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_telescope(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// Decision 0009 point 2(a): one call in 60 is refused (a Connect makes some fifty, so most land), so the poll, the
// commands and the relink stop lose the link at once while lifecycle threads connect and disconnect it and readers
// race the getters. TSan checks the loss path, the relink stop's note and send, the MoveAxis caps' start, cancel and
// join, and the write-throughs against the getters and set_connected().
TEST_CASE("Remote Alpaca telescope - refused-connection losses race connects, disconnects, commands and reads",
          "[remotealpaca][telescope][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Telescope};
    std::atomic<int> calls{0};
    http.set_responder([&upstream, &calls](const alpacacore::test::FakeHttpClient::Request& request) {
        if (calls.fetch_add(1) % 60 == 59) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_telescope(http);

    // A refused command is NotConnected (the loss is its answer); InvalidOperation and InvalidWhileParked as above.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation,
                                            alpacacore::AlpacaError::InvalidWhileParked};
    storm_until(
        *driver, [&guard](AlpacaDriver& d) { operate_telescope(guard, d); },
        [&upstream] { return upstream.calls("abortslew") > 0; });

    // The last connect() may still own its connection task (open-astro#326): settle instead of a bare Disconnect.
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // the Disconnect ended any kept reason
    CHECK(upstream.calls("abortslew") > 0);   // commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca focuser - concurrent connect/disconnect/operate stress", "[remotealpaca][focuser][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Focuser};
    upstream.install(http);
    auto driver = make_focuser(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down. InvalidOperation: a command started while a
    // Disconnect joins the command slot is refused ("try again", issue #1713).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    storm_until(
        *driver, [&guard](AlpacaDriver& d) { operate_focuser(guard, d); },
        [&upstream] { return upstream.calls("move") > 0; });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(upstream.calls("move") > 0);  // the commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca focuser - destruction races an in-flight connect", "[remotealpaca][focuser][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Focuser};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_focuser(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca focuser - racing disconnect is never dropped", "[remotealpaca][focuser][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Focuser};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_focuser(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// Decision 0009 point 2(a): one call in 40 is refused (a Connect makes some sixteen, so most land), so the poll and
// the commands lose the link at once while lifecycle threads connect and disconnect it and readers race the getters.
// TSan checks the loss path's writes (connected_, the kept reason, the dropped frame) against the getters, the Move,
// Halt and TempComp bodies and their write-throughs, and set_connected().
TEST_CASE("Remote Alpaca focuser - refused-connection losses race connects, disconnects, commands and reads",
          "[remotealpaca][focuser][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Focuser};
    std::atomic<int> calls{0};
    http.set_responder([&upstream, &calls](const alpacacore::test::FakeHttpClient::Request& request) {
        if (calls.fetch_add(1) % 40 == 39) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_focuser(http);

    // A refused command is NotConnected (the loss is its answer); InvalidOperation as above.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    storm_until(
        *driver, [&guard](AlpacaDriver& d) { operate_focuser(guard, d); },
        [&upstream] { return upstream.calls("halt") > 0; });

    // The last connect() may still own its connection task (open-astro#326): settle instead of a bare Disconnect.
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // the Disconnect ended any kept reason
    CHECK(upstream.calls("halt") > 0);        // commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca camera - concurrent connect/disconnect/operate stress", "[remotealpaca][camera][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Camera};
    upstream.install(http);
    auto driver = make_camera(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down. InvalidOperation: a command started while a
    // Disconnect joins the command slot is refused ("try again", issue #1713), the upstream's answer to a setter or a
    // StartExposure while it exposes, and ImageArray before an image.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate_camera(guard, d); });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(upstream.calls("startexposure") > 0);  // the commands ran on a connected driver, not only NotConnected
    CHECK(upstream.calls("imagearray") > 0);
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca camera - destruction races an in-flight connect", "[remotealpaca][camera][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Camera};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_camera(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca camera - racing disconnect is never dropped", "[remotealpaca][camera][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Camera};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_camera(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// Decision 0009 point 2(a): one call in 80 is refused (a Connect makes some sixty), so the poll, the commands and the
// image download lose the link at once while lifecycle threads connect and disconnect it and readers race the
// getters. TSan checks the loss path's writes against the getters, the command bodies and their write-throughs, the
// held image and its exposure count, the Connect's store and set_connected().
TEST_CASE("Remote Alpaca camera - refused-connection losses race connects, disconnects, commands and reads",
          "[remotealpaca][camera][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Camera};
    std::atomic<int> calls{0};
    std::atomic<bool> refusing{true};
    http.set_responder([&upstream, &calls, &refusing](const alpacacore::test::FakeHttpClient::Request& request) {
        if (refusing.load() && calls.fetch_add(1) % 80 == 79) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_camera(http);

    // A refused command is NotConnected (the loss is its answer); InvalidOperation as above.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate_camera(guard, d); });

    // After the storm the driver is still whole: with the refusals stopped it relinks, and a command on the known-up
    // link reaches the upstream. (Under load the storm's losses and Disconnects can refuse every command it sends,
    // so a count of the storm's own commands is no liveness check.) The last connect() may still own its connection
    // task (open-astro#326): settle instead of a bare Connect or Disconnect.
    refusing.store(false);
    REQUIRE(alpacacore::test::settle_connected(*driver, true));
    const int aborts = upstream.calls("abortexposure");
    CHECK(alpacacore::test::error_code_of([&] { driver->abort_exposure(); }) == 0);
    CHECK(upstream.calls("abortexposure") == aborts + 1);
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // the Disconnect ended any kept reason
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca dome - concurrent connect/disconnect/operate stress", "[remotealpaca][dome][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Dome};
    upstream.install(http);
    auto driver = make_dome(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down. InvalidOperation: a command started while a
    // Disconnect joins the command slot is refused ("try again", issue #1713).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate_dome(guard, d); });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(upstream.calls("slewtoazimuth") > 0);  // the commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca dome - destruction races an in-flight connect", "[remotealpaca][dome][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Dome};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_dome(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca dome - racing disconnect is never dropped", "[remotealpaca][dome][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Dome};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_dome(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// Decision 0009 point 2(a): one call in 40 is refused (a Connect makes some twenty, so most land), so the poll and the
// commands lose the link at once while lifecycle threads connect and disconnect it and readers race the getters. TSan
// checks the loss path's writes (connected_, the kept reason, the dropped frame) against the getters, the command
// bodies and their write-throughs, the Connect's capabilities and poll plan that the poll copies, and set_connected().
TEST_CASE("Remote Alpaca dome - refused-connection losses race connects, disconnects, commands and reads",
          "[remotealpaca][dome][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Dome};
    std::atomic<int> calls{0};
    http.set_responder([&upstream, &calls](const alpacacore::test::FakeHttpClient::Request& request) {
        if (calls.fetch_add(1) % 40 == 39) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_dome(http);

    // A refused command is NotConnected (the loss is its answer); InvalidOperation as above.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate_dome(guard, d); });

    // The last connect() may still own its connection task (open-astro#326): settle instead of a bare Disconnect.
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // the Disconnect ended any kept reason
    CHECK(upstream.calls("abortslew") > 0);   // commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

// The capabilities answer while disconnected and while connected, so no read of one throws, whatever Disconnect or
// Connect races it: the driver picks the answer under one state-lock hold. InvalidOperation is never a capability's
// answer; it only replaces the guard's default set, so a NotConnected is unexpected here.
TEST_CASE("Remote Alpaca dome - the capabilities never throw while connects and disconnects race them",
          "[remotealpaca][dome][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Dome};
    upstream.install(http);
    auto driver = make_dome(http);
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& dome = static_cast<alpacacore::DomeDriver&>(d);
        for (int i = 0; i < 25; ++i) {
            guard([&] { static_cast<void>(dome.get_can_set_shutter()); });
            guard([&] { static_cast<void>(dome.get_can_sync_azimuth()); });
        }
    });
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(driver->get_can_set_shutter());  // the last Connect's answer, kept while disconnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca cover calibrator - concurrent connect/disconnect/operate stress",
          "[remotealpaca][covercalibrator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::CoverCalibrator};
    upstream.install(http);
    auto driver = make_covercalibrator(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down. InvalidOperation: a command started while a
    // Disconnect joins the command slot is refused ("try again", issue #1713).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate_covercalibrator(guard, d); });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(upstream.calls("opencover") > 0);  // the commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca cover calibrator - destruction races an in-flight connect",
          "[remotealpaca][covercalibrator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::CoverCalibrator};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_covercalibrator(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca cover calibrator - racing disconnect is never dropped",
          "[remotealpaca][covercalibrator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::CoverCalibrator};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_covercalibrator(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// Decision 0009 point 2(a): one call in 40 is refused (a Connect makes some fifteen, so most land), so the poll and
// the commands lose the link at once while lifecycle threads connect and disconnect it and readers race the getters.
// TSan checks the loss path's writes (connected_, the kept reason, the dropped frame) against the getters, the five
// command bodies and their write-throughs, the parts and MaxBrightness the Connect stores for the poll and the
// getters, and set_connected().
TEST_CASE("Remote Alpaca cover calibrator - refused-connection losses race connects, disconnects, commands and reads",
          "[remotealpaca][covercalibrator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::CoverCalibrator};
    std::atomic<int> calls{0};
    http.set_responder([&upstream, &calls](const alpacacore::test::FakeHttpClient::Request& request) {
        if (calls.fetch_add(1) % 40 == 39) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_covercalibrator(http);

    // A refused command is NotConnected (the loss is its answer); InvalidOperation as above.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate_covercalibrator(guard, d); });

    // The last connect() may still own its connection task (open-astro#326): settle instead of a bare Disconnect.
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // the Disconnect ended any kept reason
    CHECK(upstream.calls("haltcover") > 0);   // commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

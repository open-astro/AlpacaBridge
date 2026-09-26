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

#include <algorithm>
#include <string>
#include <vector>

#include "catch2_compat.h"

// Issue diegopereiran/AlpacaBridge#49: a disconnected driver's DeviceState
// must be the empty list (no TimeStamp) per the ASCOM read-all FAQ: "If no
// operational states are available for the device, an empty list (with no
// TimeStamp) must be returned." Before the fix, every one of the ten device
// base classes pushed a TimeStamp entry unconditionally, so a disconnected
// driver reported [TimeStamp] instead of [].
//
// These doubles are never connected: get_connected() is always false and
// every operational getter throws NotConnected, matching a real driver's
// disconnected behavior (AGENTS.md: "Every property otherwise throws
// NotConnected when disconnected").

namespace {

[[noreturn]] void throw_not_connected() {
    throw alpacacore::AlpacaException("not connected", alpacacore::AlpacaError::NotConnected);
}

bool has_timestamp(const std::vector<alpacacore::DeviceState>& state) {
    return std::any_of(state.begin(), state.end(),
                       [](const alpacacore::DeviceState& entry) { return entry.name == "TimeStamp"; });
}

// Common AlpacaDriver plumbing shared by every disconnected double below.
// get_device_type() stays pure virtual -- each leaf class supplies its own.
template <typename Base>
class DisconnectedDriverBase : public Base {
public:
    int get_device_number() const override { return 0; }
    std::string get_name() const override { return "Disconnected Driver"; }
    std::string get_unique_id() const override { return "disconnected-driver"; }
    std::string get_description() const override { return "test double"; }
    std::string get_driver_info() const override { return "test double"; }
    std::string get_driver_version() const override { return "0.0.0"; }
    int get_interface_version() const override { return 1; }
    bool get_connected() const override { return connected_; }
    void set_connected(bool value) override { connected_ = value; }

private:
    bool connected_ = false;

public:
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return ""; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return ""; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return ""; }
};

class DisconnectedCamera : public DisconnectedDriverBase<alpacacore::CameraDriver> {
public:
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Camera; }

    int get_bayer_offset_x() const override { throw_not_connected(); }
    int get_bayer_offset_y() const override { throw_not_connected(); }
    int get_bin_x() const override { throw_not_connected(); }
    void set_bin_x(int) override {}
    int get_bin_y() const override { throw_not_connected(); }
    void set_bin_y(int) override {}
    alpacacore::CameraState get_camera_state() const override { throw_not_connected(); }
    int get_camera_x_size() const override { throw_not_connected(); }
    int get_camera_y_size() const override { throw_not_connected(); }
    bool get_can_abort_exposure() const override { throw_not_connected(); }
    bool get_can_asymmetric_bin() const override { throw_not_connected(); }
    bool get_can_fast_readout() const override { throw_not_connected(); }
    bool get_can_get_cooler_power() const override { throw_not_connected(); }
    bool get_can_pulse_guide() const override { throw_not_connected(); }
    bool get_can_set_ccd_temperature() const override { throw_not_connected(); }
    bool get_can_stop_exposure() const override { throw_not_connected(); }
    double get_ccd_temperature() const override { throw_not_connected(); }
    bool get_cooler_on() const override { throw_not_connected(); }
    void set_cooler_on(bool) override {}
    double get_cooler_power() const override { throw_not_connected(); }
    double get_electrons_per_adu() const override { throw_not_connected(); }
    double get_exposure_max() const override { throw_not_connected(); }
    double get_exposure_min() const override { throw_not_connected(); }
    double get_exposure_resolution() const override { throw_not_connected(); }
    bool get_fast_readout() const override { throw_not_connected(); }
    void set_fast_readout(bool) override {}
    double get_full_well_capacity() const override { throw_not_connected(); }
    int get_gain() const override { throw_not_connected(); }
    void set_gain(int) override {}
    int get_gain_max() const override { throw_not_connected(); }
    int get_gain_min() const override { throw_not_connected(); }
    std::vector<std::string> get_gains() const override { throw_not_connected(); }
    bool get_has_shutter() const override { throw_not_connected(); }
    double get_heat_sink_temperature() const override { throw_not_connected(); }
    alpacacore::ImageArray get_image_array() const override { throw_not_connected(); }
    std::string get_image_array_variant() const override { throw_not_connected(); }
    bool get_image_ready() const override { throw_not_connected(); }
    bool get_is_pulse_guiding() const override { throw_not_connected(); }
    double get_last_exposure_duration() const override { throw_not_connected(); }
    std::chrono::system_clock::time_point get_last_exposure_start_time() const override { throw_not_connected(); }
    int get_max_adu() const override { throw_not_connected(); }
    int get_max_bin_x() const override { throw_not_connected(); }
    int get_max_bin_y() const override { throw_not_connected(); }
    int get_num_x() const override { throw_not_connected(); }
    void set_num_x(int) override {}
    int get_num_y() const override { throw_not_connected(); }
    void set_num_y(int) override {}
    int get_offset() const override { throw_not_connected(); }
    void set_offset(int) override {}
    int get_offset_max() const override { throw_not_connected(); }
    int get_offset_min() const override { throw_not_connected(); }
    std::vector<std::string> get_offsets() const override { throw_not_connected(); }
    double get_percent_completed() const override { throw_not_connected(); }
    double get_pixel_size_x() const override { throw_not_connected(); }
    double get_pixel_size_y() const override { throw_not_connected(); }
    int get_readout_mode() const override { throw_not_connected(); }
    void set_readout_mode(int) override {}
    std::vector<std::string> get_readout_modes() const override { throw_not_connected(); }
    std::string get_sensor_name() const override { throw_not_connected(); }
    alpacacore::SensorType get_sensor_type() const override { throw_not_connected(); }
    double get_set_ccd_temperature() const override { throw_not_connected(); }
    void set_set_ccd_temperature(double) override {}
    int get_start_x() const override { throw_not_connected(); }
    void set_start_x(int) override {}
    int get_start_y() const override { throw_not_connected(); }
    void set_start_y(int) override {}
    double get_sub_exposure_duration() const override { throw_not_connected(); }
    void set_sub_exposure_duration(double) override {}
    void abort_exposure() override { throw_not_connected(); }
    void pulse_guide(int, int) override { throw_not_connected(); }
    void start_exposure(double, bool) override { throw_not_connected(); }
    void stop_exposure() override { throw_not_connected(); }
};

// Mirrors zwo_camera_driver.cpp/gphoto_camera_driver.cpp et al.: these vendor
// drivers answer CameraState and PercentCompleted with a default value while
// disconnected instead of throwing NotConnected. get_device_state() must
// still report the empty list for a disconnected driver regardless of what
// an individual vendor getter does.
class LeakyDisconnectedCamera final : public DisconnectedCamera {
public:
    alpacacore::CameraState get_camera_state() const override { return alpacacore::CameraState::Idle; }
    double get_percent_completed() const override { return 0.0; }
};

class DisconnectedCoverCalibrator final : public DisconnectedDriverBase<alpacacore::CoverCalibratorDriver> {
public:
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::CoverCalibrator; }

    int get_brightness() const override { throw_not_connected(); }
    void set_brightness(int) override {}
    alpacacore::CalibratorState get_calibrator_state() const override { throw_not_connected(); }
    bool get_calibrator_changing() const override { throw_not_connected(); }
    int get_max_brightness() const override { throw_not_connected(); }
    alpacacore::CoverState get_cover_state() const override { throw_not_connected(); }
    bool get_cover_moving() const override { throw_not_connected(); }
    void close_cover() override { throw_not_connected(); }
    void halt_cover() override { throw_not_connected(); }
    void open_cover() override { throw_not_connected(); }
    void calibrator_off() override { throw_not_connected(); }
    void calibrator_on(int) override { throw_not_connected(); }
};

class DisconnectedDome final : public DisconnectedDriverBase<alpacacore::DomeDriver> {
public:
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Dome; }

    double get_altitude() const override { throw_not_connected(); }
    bool get_can_find_home() const override { throw_not_connected(); }
    bool get_can_park() const override { throw_not_connected(); }
    bool get_can_set_altitude() const override { throw_not_connected(); }
    bool get_can_set_azimuth() const override { throw_not_connected(); }
    bool get_can_set_park() const override { throw_not_connected(); }
    bool get_can_set_shutter() const override { throw_not_connected(); }
    bool get_can_slave() const override { throw_not_connected(); }
    bool get_can_slew() const override { throw_not_connected(); }
    bool get_can_sync_azimuth() const override { throw_not_connected(); }
    double get_azimuth() const override { throw_not_connected(); }
    bool get_at_home() const override { throw_not_connected(); }
    bool get_at_park() const override { throw_not_connected(); }
    bool get_slewing() const override { throw_not_connected(); }
    int get_shutter_status() const override { throw_not_connected(); }
    bool get_slaved() const override { throw_not_connected(); }
    void set_slaved(bool) override {}
    void abort_slew() override { throw_not_connected(); }
    void close_shutter() override { throw_not_connected(); }
    void find_home() override { throw_not_connected(); }
    void open_shutter() override { throw_not_connected(); }
    void park() override { throw_not_connected(); }
    void set_park() override { throw_not_connected(); }
    void slew_to_azimuth(double) override { throw_not_connected(); }
    void slew_to_altitude(double) override { throw_not_connected(); }
    void sync_to_azimuth(double) override { throw_not_connected(); }
};

class DisconnectedFilterWheel final : public DisconnectedDriverBase<alpacacore::FilterWheelDriver> {
public:
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::FilterWheel; }

    int get_position() const override { throw_not_connected(); }
    void set_position(int) override {}
    std::vector<int> get_focus_offsets() const override { throw_not_connected(); }
    void set_focus_offsets(const std::vector<int>&) override {}
    std::vector<std::string> get_names() const override { throw_not_connected(); }
    void set_names(const std::vector<std::string>&) override {}
};

class DisconnectedFocuser final : public DisconnectedDriverBase<alpacacore::FocuserDriver> {
public:
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Focuser; }

    bool get_absolute() const override { throw_not_connected(); }
    bool get_is_moving() const override { throw_not_connected(); }
    int get_max_step() const override { throw_not_connected(); }
    int get_max_increment() const override { throw_not_connected(); }
    int get_position() const override { throw_not_connected(); }
    double get_step_size() const override { throw_not_connected(); }
    bool get_temp_comp_available() const override { throw_not_connected(); }
    bool get_temp_comp() const override { throw_not_connected(); }
    void set_temp_comp(bool) override {}
    double get_temperature() const override { throw_not_connected(); }
    void halt() override { throw_not_connected(); }
    void move(int) override { throw_not_connected(); }
};

class DisconnectedObservingConditions final : public DisconnectedDriverBase<alpacacore::ObservingConditionsDriver> {
public:
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::ObservingConditions; }

    double get_average_period() const override { throw_not_connected(); }
    void set_average_period(double) override {}
    double get_cloud_cover() const override { throw_not_connected(); }
    double get_dew_point() const override { throw_not_connected(); }
    double get_humidity() const override { throw_not_connected(); }
    double get_pressure() const override { throw_not_connected(); }
    double get_rain_rate() const override { throw_not_connected(); }
    double get_sky_brightness() const override { throw_not_connected(); }
    double get_sky_quality() const override { throw_not_connected(); }
    double get_sky_temperature() const override { throw_not_connected(); }
    double get_seeing() const override { throw_not_connected(); }
    double get_star_fwhm() const override { throw_not_connected(); }
    double get_temperature() const override { throw_not_connected(); }
    double get_wind_direction() const override { throw_not_connected(); }
    double get_wind_gust() const override { throw_not_connected(); }
    double get_wind_speed() const override { throw_not_connected(); }
    double get_time_since_last_update(std::string_view) const override { throw_not_connected(); }
    std::string get_sensor_description(std::string_view) const override { throw_not_connected(); }
    void refresh() override { throw_not_connected(); }
};

class DisconnectedRotator final : public DisconnectedDriverBase<alpacacore::RotatorDriver> {
public:
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Rotator; }

    bool get_can_reverse() const override { throw_not_connected(); }
    bool get_reverse() const override { throw_not_connected(); }
    void set_reverse(bool) override {}
    bool get_is_moving() const override { throw_not_connected(); }
    double get_mechanical_position() const override { throw_not_connected(); }
    double get_position() const override { throw_not_connected(); }
    double get_step_size() const override { throw_not_connected(); }
    double get_target_position() const override { throw_not_connected(); }
    void set_target_position(double) override {}
    void halt() override { throw_not_connected(); }
    void move(double) override { throw_not_connected(); }
    void move_absolute(double) override { throw_not_connected(); }
    void move_mechanical(double) override { throw_not_connected(); }
    void sync(double) override { throw_not_connected(); }
};

class DisconnectedSafetyMonitor final : public DisconnectedDriverBase<alpacacore::SafetyMonitorDriver> {
public:
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::SafetyMonitor; }

    bool get_is_safe() const override { throw_not_connected(); }
};

class DisconnectedSwitch final : public DisconnectedDriverBase<alpacacore::SwitchDriver> {
public:
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Switch; }

    int get_max_switch() const override { throw_not_connected(); }
    bool get_can_write(int) const override { throw_not_connected(); }
    bool get_can_async(int) const override { throw_not_connected(); }
    bool get_switch(int) const override { throw_not_connected(); }
    void set_switch(int, bool) override {}
    void set_async(int, bool) override {}
    double get_switch_value(int) const override { throw_not_connected(); }
    void set_switch_value(int, double) override {}
    void set_async_value(int, double) override {}
    bool get_state_change_complete(int) const override { throw_not_connected(); }
    std::string get_switch_name(int) const override { throw_not_connected(); }
    void set_switch_name(int, const std::string&) override {}
    std::string get_switch_description(int) const override { throw_not_connected(); }
    double get_min_switch_value(int) const override { throw_not_connected(); }
    double get_max_switch_value(int) const override { throw_not_connected(); }
    double get_switch_step(int) const override { throw_not_connected(); }
};

class DisconnectedTelescope : public DisconnectedDriverBase<alpacacore::TelescopeDriver> {
public:
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Telescope; }

    alpacacore::AlignmentMode get_alignment_mode() const override { throw_not_connected(); }
    double get_altitude() const override { throw_not_connected(); }
    double get_aperture_diameter() const override { throw_not_connected(); }
    void set_aperture_diameter(double) override {}
    double get_aperture_area() const override { throw_not_connected(); }
    bool get_at_home() const override { throw_not_connected(); }
    bool get_at_park() const override { throw_not_connected(); }
    double get_azimuth() const override { throw_not_connected(); }
    bool get_can_find_home() const override { throw_not_connected(); }
    bool get_can_park() const override { throw_not_connected(); }
    bool get_can_pulse_guide() const override { throw_not_connected(); }
    bool get_is_pulse_guiding() const override { throw_not_connected(); }
    bool get_can_set_declination_rate() const override { throw_not_connected(); }
    bool get_can_set_guide_rates() const override { throw_not_connected(); }
    bool get_can_set_park() const override { throw_not_connected(); }
    bool get_can_set_pier_side() const override { throw_not_connected(); }
    bool get_can_set_right_ascension_rate() const override { throw_not_connected(); }
    bool get_can_set_tracking() const override { throw_not_connected(); }
    bool get_can_slew_alt_az() const override { throw_not_connected(); }
    bool get_can_slew_alt_az_async() const override { throw_not_connected(); }
    bool get_can_sync_alt_az() const override { throw_not_connected(); }
    bool get_can_slew() const override { throw_not_connected(); }
    bool get_can_slew_async() const override { throw_not_connected(); }
    bool get_can_sync() const override { throw_not_connected(); }
    bool get_can_unpark() const override { throw_not_connected(); }
    double get_declination() const override { throw_not_connected(); }
    double get_declination_rate() const override { throw_not_connected(); }
    void set_declination_rate(double) override {}
    bool get_tracking() const override { throw_not_connected(); }
    void set_tracking(bool) override {}
    double get_focal_length() const override { throw_not_connected(); }
    void set_focal_length(double) override {}
    alpacacore::GuideRate get_guide_rate() const override { throw_not_connected(); }
    void set_guide_rate(const alpacacore::GuideRate&) override {}
    double get_right_ascension() const override { throw_not_connected(); }
    double get_right_ascension_rate() const override { throw_not_connected(); }
    void set_right_ascension_rate(double) override {}
    int get_side_of_pier() const override { throw_not_connected(); }
    void set_side_of_pier(int) override {}
    int get_destination_side_of_pier(double, double) const override { throw_not_connected(); }
    alpacacore::EquatorialSystem get_equatorial_system() const override { throw_not_connected(); }
    bool get_does_refraction() const override { throw_not_connected(); }
    void set_does_refraction(bool) override {}
    int get_slew_settle_time() const override { throw_not_connected(); }
    void set_slew_settle_time(int) override {}
    double get_sidereal_time() const override { throw_not_connected(); }
    double get_site_elevation() const override { throw_not_connected(); }
    void set_site_elevation(double) override {}
    double get_site_latitude() const override { throw_not_connected(); }
    void set_site_latitude(double) override {}
    double get_site_longitude() const override { throw_not_connected(); }
    void set_site_longitude(double) override {}
    bool get_slewing() const override { throw_not_connected(); }
    double get_target_declination() const override { throw_not_connected(); }
    void set_target_declination(double) override {}
    double get_target_right_ascension() const override { throw_not_connected(); }
    void set_target_right_ascension(double) override {}
    int get_tracking_rate() const override { throw_not_connected(); }
    void set_tracking_rate(int) override {}
    std::vector<int> get_tracking_rates() const override { throw_not_connected(); }
    std::chrono::system_clock::time_point get_utc_date() const override { throw_not_connected(); }
    void set_utc_date(std::chrono::system_clock::time_point) override {}
    void find_home() override { throw_not_connected(); }
    void park() override { throw_not_connected(); }
    void pulse_guide(int, int) override { throw_not_connected(); }
    void set_park() override { throw_not_connected(); }
    void slew_to_coordinates(double, double) override { throw_not_connected(); }
    void slew_to_coordinates_async(double, double) override { throw_not_connected(); }
    void slew_to_target() override { throw_not_connected(); }
    void slew_to_target_async() override { throw_not_connected(); }
    void sync_to_coordinates(double, double) override { throw_not_connected(); }
    void sync_to_target() override { throw_not_connected(); }
    void unpark() override { throw_not_connected(); }
    bool get_can_move_axis(int) const override { throw_not_connected(); }
    void move_axis(int, double) override { throw_not_connected(); }
    std::pair<double, double> get_axis_rate_range(int) const override { throw_not_connected(); }
    void abort_slew() override { throw_not_connected(); }
    void slew_to_alt_az(double, double) override { throw_not_connected(); }
    void slew_to_alt_az_async(double, double) override { throw_not_connected(); }
    void sync_to_alt_az(double, double) override { throw_not_connected(); }
};

// Synthetic: no telescope driver answers AtPark with a cached bool while
// disconnected any more (open-astro#656), and the tier-1 contract sweep probes
// get_at_park/get_at_home on every telescope. This fixture keeps
// get_device_state() reporting the empty list for a disconnected driver even
// if an individual getter does not throw.
class LeakyDisconnectedTelescope final : public DisconnectedTelescope {
public:
    bool get_at_park() const override { return false; }
};

}  // namespace

TEST_CASE("DeviceState is empty while disconnected - Camera", "[driver]") {
    DisconnectedCamera driver;
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(state.empty());
    CHECK_FALSE(has_timestamp(state));
}

TEST_CASE("DeviceState is empty while disconnected even when a getter does not throw - Camera", "[driver]") {
    LeakyDisconnectedCamera driver;
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(state.empty());
}

TEST_CASE("DeviceState is empty while disconnected - CoverCalibrator", "[driver]") {
    DisconnectedCoverCalibrator driver;
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(state.empty());
    CHECK_FALSE(has_timestamp(state));
}

TEST_CASE("DeviceState is empty while disconnected - Dome", "[driver]") {
    DisconnectedDome driver;
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(state.empty());
    CHECK_FALSE(has_timestamp(state));
}

TEST_CASE("DeviceState is empty while disconnected - FilterWheel", "[driver]") {
    DisconnectedFilterWheel driver;
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(state.empty());
    CHECK_FALSE(has_timestamp(state));
}

TEST_CASE("DeviceState is empty while disconnected - Focuser", "[driver]") {
    DisconnectedFocuser driver;
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(state.empty());
    CHECK_FALSE(has_timestamp(state));
}

TEST_CASE("DeviceState is empty while disconnected - ObservingConditions", "[driver]") {
    DisconnectedObservingConditions driver;
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(state.empty());
    CHECK_FALSE(has_timestamp(state));
}

TEST_CASE("DeviceState is empty while disconnected - Rotator", "[driver]") {
    DisconnectedRotator driver;
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(state.empty());
    CHECK_FALSE(has_timestamp(state));
}

TEST_CASE("DeviceState is empty while disconnected - SafetyMonitor", "[driver]") {
    DisconnectedSafetyMonitor driver;
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(state.empty());
    CHECK_FALSE(has_timestamp(state));
}

TEST_CASE("DeviceState is empty while disconnected - Switch", "[driver]") {
    DisconnectedSwitch driver;
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(state.empty());
    CHECK_FALSE(has_timestamp(state));
}

TEST_CASE("DeviceState is empty while disconnected - Telescope", "[driver]") {
    DisconnectedTelescope driver;
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(state.empty());
    CHECK_FALSE(has_timestamp(state));
}

TEST_CASE("DeviceState is empty while disconnected even when a getter does not throw - Telescope", "[driver]") {
    LeakyDisconnectedTelescope driver;
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(state.empty());
}

// The connected side of the contract: the early return must key on
// get_connected(), not be unconditional. Each double's getters throw
// NotConnected regardless (omitted by the per-getter try/catch), so a
// connected double still reports the TimeStamp -- and only a base class whose
// early return ignored get_connected() could report the empty list here.
namespace {
template <typename Driver>
void require_timestamp_once_connected() {
    Driver driver;
    REQUIRE(driver.get_device_state().empty());
    driver.set_connected(true);
    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = driver.get_device_state());
    CHECK(has_timestamp(state));
}
}  // namespace

TEST_CASE("DeviceState carries the TimeStamp once connected, per base class", "[driver]") {
    SECTION("Camera") { require_timestamp_once_connected<DisconnectedCamera>(); }
    SECTION("CoverCalibrator") { require_timestamp_once_connected<DisconnectedCoverCalibrator>(); }
    SECTION("Dome") { require_timestamp_once_connected<DisconnectedDome>(); }
    SECTION("FilterWheel") { require_timestamp_once_connected<DisconnectedFilterWheel>(); }
    SECTION("Focuser") { require_timestamp_once_connected<DisconnectedFocuser>(); }
    SECTION("ObservingConditions") { require_timestamp_once_connected<DisconnectedObservingConditions>(); }
    SECTION("Rotator") { require_timestamp_once_connected<DisconnectedRotator>(); }
    SECTION("SafetyMonitor") { require_timestamp_once_connected<DisconnectedSafetyMonitor>(); }
    SECTION("Switch") { require_timestamp_once_connected<DisconnectedSwitch>(); }
    SECTION("Telescope") { require_timestamp_once_connected<DisconnectedTelescope>(); }
}

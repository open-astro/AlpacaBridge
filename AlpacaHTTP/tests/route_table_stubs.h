// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

// One do-nothing driver per Alpaca device type, used by test_route_table to
// reach the router's per-type dispatchers (#646). Every pure virtual of the
// device interface returns a value-initialised default; nothing here models
// device behaviour. If a driver interface gains or loses a pure virtual this
// header stops compiling, which is the cue to update it deliberately.

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/alpacadriver.h>
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

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace route_table_stubs {

using namespace alpacacore;

// The AlpacaDriver members every stub shares.
#define ROUTE_TABLE_STUB_COMMON(DEVICE_TYPE, LABEL)                                                         \
    int get_device_number() const override { return number_; }                                              \
    std::string get_name() const override { return "Route Table Stub " LABEL; }                             \
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::DEVICE_TYPE; } \
    std::string get_unique_id() const override { return "route-table-stub-" LABEL; }                        \
    std::string get_description() const override { return "fake device"; }                                  \
    std::string get_driver_info() const override { return "fake driver"; }                                  \
    std::string get_driver_version() const override { return "0.0.1"; }                                     \
    int get_interface_version() const override { return 1; }                                                \
    bool get_connected() const override { return connected_; }                                              \
    void set_connected(bool connected) override { connected_ = connected; }                                 \
    std::vector<std::string> get_supported_actions() const override { return {}; }                          \
    std::string action(std::string_view, std::string_view) override { return {}; }                          \
    bool can_action(std::string_view) const override { return false; }                                      \
    std::string command_blind(std::string_view, bool) override { return {}; }                               \
    bool command_bool(std::string_view, bool) override { return false; }                                    \
    std::string command_string(std::string_view, bool) override { return {}; }

class CameraStub final : public alpacacore::CameraDriver {
public:
    explicit CameraStub(int number) : number_(number) {}
    ROUTE_TABLE_STUB_COMMON(Camera, "camera")

    int get_bayer_offset_x() const override { return int{}; }
    int get_bayer_offset_y() const override { return int{}; }
    int get_bin_x() const override { return int{}; }
    void set_bin_x(int bin_x) override {}
    int get_bin_y() const override { return int{}; }
    void set_bin_y(int bin_y) override {}
    CameraState get_camera_state() const override { return CameraState{}; }
    int get_camera_x_size() const override { return int{}; }
    int get_camera_y_size() const override { return int{}; }
    bool get_can_abort_exposure() const override { return bool{}; }
    bool get_can_asymmetric_bin() const override { return bool{}; }
    bool get_can_fast_readout() const override { return bool{}; }
    bool get_can_get_cooler_power() const override { return bool{}; }
    bool get_can_pulse_guide() const override { return bool{}; }
    bool get_can_set_ccd_temperature() const override { return bool{}; }
    bool get_can_stop_exposure() const override { return bool{}; }
    double get_ccd_temperature() const override { return double{}; }
    bool get_cooler_on() const override { return bool{}; }
    void set_cooler_on(bool cooler_on) override {}
    double get_cooler_power() const override { return double{}; }
    double get_electrons_per_adu() const override { return double{}; }
    double get_exposure_max() const override { return double{}; }
    double get_exposure_min() const override { return double{}; }
    double get_exposure_resolution() const override { return double{}; }
    bool get_fast_readout() const override { return bool{}; }
    void set_fast_readout(bool fast_readout) override {}
    double get_full_well_capacity() const override { return double{}; }
    int get_gain() const override { return int{}; }
    void set_gain(int gain) override {}
    int get_gain_max() const override { return int{}; }
    int get_gain_min() const override { return int{}; }
    std::vector<std::string> get_gains() const override { return std::vector<std::string>{}; }
    bool get_has_shutter() const override { return bool{}; }
    double get_heat_sink_temperature() const override { return double{}; }
    ImageArray get_image_array() const override { return ImageArray{}; }
    std::string get_image_array_variant() const override { return std::string{}; }
    bool get_image_ready() const override { return bool{}; }
    bool get_is_pulse_guiding() const override { return bool{}; }
    double get_last_exposure_duration() const override { return double{}; }
    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        return std::chrono::system_clock::time_point{};
    }
    int get_max_adu() const override { return int{}; }
    int get_max_bin_x() const override { return int{}; }
    int get_max_bin_y() const override { return int{}; }
    int get_num_x() const override { return int{}; }
    void set_num_x(int num_x) override {}
    int get_num_y() const override { return int{}; }
    void set_num_y(int num_y) override {}
    int get_offset() const override { return int{}; }
    void set_offset(int offset) override {}
    int get_offset_max() const override { return int{}; }
    int get_offset_min() const override { return int{}; }
    std::vector<std::string> get_offsets() const override { return std::vector<std::string>{}; }
    double get_percent_completed() const override { return double{}; }
    double get_pixel_size_x() const override { return double{}; }
    double get_pixel_size_y() const override { return double{}; }
    int get_readout_mode() const override { return int{}; }
    void set_readout_mode(int mode) override {}
    std::vector<std::string> get_readout_modes() const override { return std::vector<std::string>{}; }
    std::string get_sensor_name() const override { return std::string{}; }
    SensorType get_sensor_type() const override { return SensorType{}; }
    double get_set_ccd_temperature() const override { return double{}; }
    void set_set_ccd_temperature(double temperature) override {}
    int get_start_x() const override { return int{}; }
    void set_start_x(int start_x) override {}
    int get_start_y() const override { return int{}; }
    void set_start_y(int start_y) override {}
    double get_sub_exposure_duration() const override { return double{}; }
    void set_sub_exposure_duration(double duration) override {}
    void abort_exposure() override {}
    void pulse_guide(int direction, int duration) override {}
    void start_exposure(double duration, bool light) override {}
    void stop_exposure() override {}

private:
    int number_;
    bool connected_ = false;
};

class TelescopeStub final : public alpacacore::TelescopeDriver {
public:
    explicit TelescopeStub(int number) : number_(number) {}
    ROUTE_TABLE_STUB_COMMON(Telescope, "telescope")

    AlignmentMode get_alignment_mode() const override { return AlignmentMode{}; }
    double get_altitude() const override { return double{}; }
    double get_aperture_diameter() const override { return double{}; }
    void set_aperture_diameter(double meters) override {}
    double get_aperture_area() const override { return double{}; }
    bool get_at_home() const override { return bool{}; }
    bool get_at_park() const override { return bool{}; }
    double get_azimuth() const override { return double{}; }
    bool get_can_find_home() const override { return bool{}; }
    bool get_can_park() const override { return bool{}; }
    bool get_can_pulse_guide() const override { return bool{}; }
    bool get_is_pulse_guiding() const override { return bool{}; }
    bool get_can_set_declination_rate() const override { return bool{}; }
    bool get_can_set_guide_rates() const override { return bool{}; }
    bool get_can_set_park() const override { return bool{}; }
    bool get_can_set_pier_side() const override { return bool{}; }
    bool get_can_set_right_ascension_rate() const override { return bool{}; }
    bool get_can_set_tracking() const override { return bool{}; }
    bool get_can_slew_alt_az() const override { return bool{}; }
    bool get_can_slew_alt_az_async() const override { return bool{}; }
    bool get_can_sync_alt_az() const override { return bool{}; }
    bool get_can_slew() const override { return bool{}; }
    bool get_can_slew_async() const override { return bool{}; }
    bool get_can_sync() const override { return bool{}; }
    bool get_can_unpark() const override { return bool{}; }
    double get_declination() const override { return double{}; }
    double get_declination_rate() const override { return double{}; }
    void set_declination_rate(double rate) override {}
    bool get_tracking() const override { return bool{}; }
    void set_tracking(bool tracking) override {}
    double get_focal_length() const override { return double{}; }
    void set_focal_length(double meters) override {}
    GuideRate get_guide_rate() const override { return GuideRate{}; }
    void set_guide_rate(const GuideRate& rate) override {}
    double get_right_ascension() const override { return double{}; }
    double get_right_ascension_rate() const override { return double{}; }
    void set_right_ascension_rate(double rate) override {}
    int get_side_of_pier() const override { return int{}; }
    void set_side_of_pier(int side) override {}
    int get_destination_side_of_pier(double ra, double dec) const override { return int{}; }
    EquatorialSystem get_equatorial_system() const override { return EquatorialSystem{}; }
    bool get_does_refraction() const override { return bool{}; }
    void set_does_refraction(bool does_refraction) override {}
    int get_slew_settle_time() const override { return int{}; }
    void set_slew_settle_time(int seconds) override {}
    double get_sidereal_time() const override { return double{}; }
    double get_site_elevation() const override { return double{}; }
    void set_site_elevation(double elevation) override {}
    double get_site_latitude() const override { return double{}; }
    void set_site_latitude(double latitude) override {}
    double get_site_longitude() const override { return double{}; }
    void set_site_longitude(double longitude) override {}
    bool get_slewing() const override { return bool{}; }
    double get_target_declination() const override { return double{}; }
    void set_target_declination(double dec) override {}
    double get_target_right_ascension() const override { return double{}; }
    void set_target_right_ascension(double ra) override {}
    int get_tracking_rate() const override { return int{}; }
    void set_tracking_rate(int rate) override {}
    std::vector<int> get_tracking_rates() const override { return std::vector<int>{}; }
    std::chrono::system_clock::time_point get_utc_date() const override {
        return std::chrono::system_clock::time_point{};
    }
    void set_utc_date(std::chrono::system_clock::time_point utc) override {}
    void find_home() override {}
    void park() override {}
    void pulse_guide(int direction, int duration) override {}
    void set_park() override {}
    void slew_to_coordinates(double ra, double dec) override {}
    void slew_to_coordinates_async(double ra, double dec) override {}
    void slew_to_target() override {}
    void slew_to_target_async() override {}
    void sync_to_coordinates(double ra, double dec) override {}
    void sync_to_target() override {}
    void unpark() override {}
    bool get_can_move_axis(int axis) const override { return bool{}; }
    void move_axis(int axis, double rate) override {}
    std::pair<double, double> get_axis_rate_range(int axis) const override { return std::pair<double, double>{}; }
    void abort_slew() override {}
    void slew_to_alt_az(double altitude, double azimuth) override {}
    void slew_to_alt_az_async(double altitude, double azimuth) override {}
    void sync_to_alt_az(double altitude, double azimuth) override {}

private:
    int number_;
    bool connected_ = false;
};

class FilterWheelStub final : public alpacacore::FilterWheelDriver {
public:
    explicit FilterWheelStub(int number) : number_(number) {}
    ROUTE_TABLE_STUB_COMMON(FilterWheel, "filterwheel")

    int get_position() const override { return int{}; }
    void set_position(int position) override {}
    std::vector<int> get_focus_offsets() const override { return std::vector<int>{}; }
    void set_focus_offsets(const std::vector<int>& offsets) override {}
    std::vector<std::string> get_names() const override { return std::vector<std::string>{}; }
    void set_names(const std::vector<std::string>& names) override {}

private:
    int number_;
    bool connected_ = false;
};

class FocuserStub final : public alpacacore::FocuserDriver {
public:
    explicit FocuserStub(int number) : number_(number) {}
    ROUTE_TABLE_STUB_COMMON(Focuser, "focuser")

    bool get_absolute() const override { return bool{}; }
    bool get_is_moving() const override { return bool{}; }
    int get_max_step() const override { return int{}; }
    int get_max_increment() const override { return int{}; }
    int get_position() const override { return int{}; }
    double get_step_size() const override { return double{}; }
    bool get_temp_comp_available() const override { return bool{}; }
    bool get_temp_comp() const override { return bool{}; }
    void set_temp_comp(bool temp_comp) override {}
    double get_temperature() const override { return double{}; }
    void halt() override {}
    void move(int position) override {}

private:
    int number_;
    bool connected_ = false;
};

class RotatorStub final : public alpacacore::RotatorDriver {
public:
    explicit RotatorStub(int number) : number_(number) {}
    ROUTE_TABLE_STUB_COMMON(Rotator, "rotator")

    bool get_can_reverse() const override { return bool{}; }
    bool get_reverse() const override { return bool{}; }
    void set_reverse(bool reverse) override {}
    bool get_is_moving() const override { return bool{}; }
    double get_mechanical_position() const override { return double{}; }
    double get_position() const override { return double{}; }
    double get_step_size() const override { return double{}; }
    double get_target_position() const override { return double{}; }
    void set_target_position(double position) override {}
    void halt() override {}
    void move(double position) override {}
    void move_absolute(double position) override {}
    void move_mechanical(double position) override {}
    void sync(double position) override {}

private:
    int number_;
    bool connected_ = false;
};

class DomeStub final : public alpacacore::DomeDriver {
public:
    explicit DomeStub(int number) : number_(number) {}
    ROUTE_TABLE_STUB_COMMON(Dome, "dome")

    double get_altitude() const override { return double{}; }
    bool get_can_find_home() const override { return bool{}; }
    bool get_can_park() const override { return bool{}; }
    bool get_can_set_altitude() const override { return bool{}; }
    bool get_can_set_azimuth() const override { return bool{}; }
    bool get_can_set_park() const override { return bool{}; }
    bool get_can_set_shutter() const override { return bool{}; }
    bool get_can_slave() const override { return bool{}; }
    bool get_can_slew() const override { return bool{}; }
    bool get_can_sync_azimuth() const override { return bool{}; }
    double get_azimuth() const override { return double{}; }
    bool get_at_home() const override { return bool{}; }
    bool get_at_park() const override { return bool{}; }
    bool get_slewing() const override { return bool{}; }
    int get_shutter_status() const override { return int{}; }
    bool get_slaved() const override { return bool{}; }
    void set_slaved(bool slaved) override {}
    void abort_slew() override {}
    void close_shutter() override {}
    void find_home() override {}
    void open_shutter() override {}
    void park() override {}
    void set_park() override {}
    void slew_to_azimuth(double azimuth) override {}
    void slew_to_altitude(double altitude) override {}
    void sync_to_azimuth(double azimuth) override {}

private:
    int number_;
    bool connected_ = false;
};

class SwitchStub final : public alpacacore::SwitchDriver {
public:
    explicit SwitchStub(int number) : number_(number) {}
    ROUTE_TABLE_STUB_COMMON(Switch, "switch")

    int get_max_switch() const override { return int{}; }
    bool get_can_write(int id) const override { return bool{}; }
    bool get_can_async(int id) const override { return bool{}; }
    bool get_switch(int id) const override { return bool{}; }
    void set_switch(int id, bool state) override {}
    void set_async(int id, bool state) override {}
    double get_switch_value(int id) const override { return double{}; }
    void set_switch_value(int id, double value) override {}
    void set_async_value(int id, double value) override {}
    bool get_state_change_complete(int id) const override { return bool{}; }
    std::string get_switch_name(int id) const override { return std::string{}; }
    void set_switch_name(int id, const std::string& name) override {}
    std::string get_switch_description(int id) const override { return std::string{}; }
    double get_min_switch_value(int id) const override { return double{}; }
    double get_max_switch_value(int id) const override { return double{}; }
    double get_switch_step(int id) const override { return double{}; }

private:
    int number_;
    bool connected_ = false;
};

class CoverCalibratorStub final : public alpacacore::CoverCalibratorDriver {
public:
    explicit CoverCalibratorStub(int number) : number_(number) {}
    ROUTE_TABLE_STUB_COMMON(CoverCalibrator, "covercalibrator")

    int get_brightness() const override { return int{}; }
    void set_brightness(int brightness) override {}
    CalibratorState get_calibrator_state() const override { return CalibratorState{}; }
    bool get_calibrator_changing() const override { return bool{}; }
    int get_max_brightness() const override { return int{}; }
    CoverState get_cover_state() const override { return CoverState{}; }
    bool get_cover_moving() const override { return bool{}; }
    void close_cover() override {}
    void halt_cover() override {}
    void open_cover() override {}
    void calibrator_off() override {}
    void calibrator_on(int brightness) override {}

private:
    int number_;
    bool connected_ = false;
};

class ObservingConditionsStub final : public alpacacore::ObservingConditionsDriver {
public:
    explicit ObservingConditionsStub(int number) : number_(number) {}
    ROUTE_TABLE_STUB_COMMON(ObservingConditions, "observingconditions")

    double get_average_period() const override { return double{}; }
    void set_average_period(double period) override {}
    double get_cloud_cover() const override { return double{}; }
    double get_dew_point() const override { return double{}; }
    double get_humidity() const override { return double{}; }
    double get_pressure() const override { return double{}; }
    double get_rain_rate() const override { return double{}; }
    double get_sky_brightness() const override { return double{}; }
    double get_sky_quality() const override { return double{}; }
    double get_sky_temperature() const override { return double{}; }
    double get_seeing() const override { return double{}; }
    double get_star_fwhm() const override { return double{}; }
    double get_temperature() const override { return double{}; }
    double get_wind_direction() const override { return double{}; }
    double get_wind_gust() const override { return double{}; }
    double get_wind_speed() const override { return double{}; }
    double get_time_since_last_update(std::string_view property_name) const override { return double{}; }
    std::string get_sensor_description(std::string_view property_name) const override { return std::string{}; }
    void refresh() override {}

private:
    int number_;
    bool connected_ = false;
};

class SafetyMonitorStub final : public alpacacore::SafetyMonitorDriver {
public:
    explicit SafetyMonitorStub(int number) : number_(number) {}
    ROUTE_TABLE_STUB_COMMON(SafetyMonitor, "safetymonitor")

    bool get_is_safe() const override { return bool{}; }

private:
    int number_;
    bool connected_ = false;
};

#undef ROUTE_TABLE_STUB_COMMON

}  // namespace route_table_stubs

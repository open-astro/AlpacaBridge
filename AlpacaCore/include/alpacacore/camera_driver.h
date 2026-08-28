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

#pragma once

#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/error_handling.h>
#include <cstdint>
#include <vector>
#include <chrono>
#include <string>

namespace alpacacore {

/**
 * @brief Camera sensor type.
 */
enum class SensorType {
    Monochrome,
    Color,
    RGGB,
    CMYG,
    CMYG2,
    LRGB
};

/**
 * @brief Camera operational state.
 */
enum class CameraState {
    Idle = 0,
    Waiting = 1,
    Exposing = 2,
    Reading = 3,
    Download = 4,
    Error = 5
};

/**
 * @brief Camera image array structure.
 */
struct ImageArray {
    std::vector<std::int32_t> data;
    int width{};
    int height{};
    int rank{};  // 2 for monochrome, 3 for color
};

/**
 * @brief Pure virtual interface for Alpaca Camera drivers.
 *
 * Follows ASCOM Alpaca Camera API specification.
 * All camera drivers must implement this interface.
 */
class CameraDriver : public AlpacaDriver {
public:
    virtual ~CameraDriver() = default;

    // Platform 7 operational state (ICameraV4). Built once here from the
    // individual property getters so the reported values always agree with the
    // corresponding GET endpoints (a consistency ConformU verifies). Properties
    // that throw (not implemented / not currently known) are omitted; a
    // TimeStamp entry is always added. Defined inline to keep the CameraDriver
    // vtable weak so the per-vendor static libraries link without a base-library
    // ordering dependency.
    std::vector<DeviceState> get_device_state() const override final {
        std::vector<DeviceState> state;

        auto add = [&state](const char* name, auto getter) {
            try {
                state.push_back({name, DeviceStateValue{getter()}});
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                // Not currently known -- or an unwrapped vendor error -- so omit per the DeviceState contract.
            }
        };

        add("CameraState", [this] { return static_cast<std::int32_t>(get_camera_state()); });
        add("CCDTemperature", [this] { return get_ccd_temperature(); });
        add("CoolerPower", [this] { return get_cooler_power(); });
        add("HeatSinkTemperature", [this] { return get_heat_sink_temperature(); });
        add("ImageReady", [this] { return get_image_ready(); });
        add("IsPulseGuiding", [this] { return get_is_pulse_guiding(); });
        // ICameraV4.PercentCompleted is a short: emit an integer, matching the
        // GET endpoint. As a double ("0.0") ConformU cannot coerce it and
        // reports the property "not included in the DeviceState response".
        add("PercentCompleted", [this] { return static_cast<std::int32_t>(get_percent_completed()); });

        state.push_back({"TimeStamp", device_state_timestamp()});
        return state;
    }

    // Camera-specific properties

    virtual int get_bayer_offset_x() const = 0;
    virtual int get_bayer_offset_y() const = 0;

    virtual int get_bin_x() const = 0;
    virtual void set_bin_x(int bin_x) = 0;

    virtual int get_bin_y() const = 0;
    virtual void set_bin_y(int bin_y) = 0;

    virtual CameraState get_camera_state() const = 0;
    virtual int get_camera_x_size() const = 0;
    virtual int get_camera_y_size() const = 0;

    virtual bool get_can_abort_exposure() const = 0;
    virtual bool get_can_asymmetric_bin() const = 0;
    virtual bool get_can_fast_readout() const = 0;
    virtual bool get_can_get_cooler_power() const = 0;
    virtual bool get_can_pulse_guide() const = 0;
    virtual bool get_can_set_ccd_temperature() const = 0;
    virtual bool get_can_stop_exposure() const = 0;

    virtual double get_ccd_temperature() const = 0;

    virtual bool get_cooler_on() const = 0;
    virtual void set_cooler_on(bool cooler_on) = 0;

    virtual double get_cooler_power() const = 0;

    virtual double get_electrons_per_adu() const = 0;
    virtual double get_exposure_max() const = 0;
    virtual double get_exposure_min() const = 0;
    virtual double get_exposure_resolution() const = 0;

    virtual bool get_fast_readout() const = 0;
    virtual void set_fast_readout(bool fast_readout) = 0;

    virtual double get_full_well_capacity() const = 0;

    virtual int get_gain() const = 0;
    virtual void set_gain(int gain) = 0;
    virtual int get_gain_max() const = 0;
    virtual int get_gain_min() const = 0;
    virtual std::vector<std::string> get_gains() const = 0;

    virtual bool get_has_shutter() const = 0;

    virtual double get_heat_sink_temperature() const = 0;

    virtual ImageArray get_image_array() const = 0;
    virtual std::string get_image_array_variant() const = 0;

    virtual bool get_image_ready() const = 0;
    virtual bool get_is_pulse_guiding() const = 0;

    virtual double get_last_exposure_duration() const = 0;
    virtual std::chrono::system_clock::time_point get_last_exposure_start_time() const = 0;

    virtual int get_max_adu() const = 0;
    virtual int get_max_bin_x() const = 0;
    virtual int get_max_bin_y() const = 0;

    virtual int get_num_x() const = 0;
    virtual void set_num_x(int num_x) = 0;

    virtual int get_num_y() const = 0;
    virtual void set_num_y(int num_y) = 0;

    virtual int get_offset() const = 0;
    virtual void set_offset(int offset) = 0;
    virtual int get_offset_max() const = 0;
    virtual int get_offset_min() const = 0;
    virtual std::vector<std::string> get_offsets() const = 0;

    virtual double get_percent_completed() const = 0;

    virtual double get_pixel_size_x() const = 0;
    virtual double get_pixel_size_y() const = 0;

    virtual int get_readout_mode() const = 0;
    virtual void set_readout_mode(int mode) = 0;
    virtual std::vector<std::string> get_readout_modes() const = 0;

    virtual std::string get_sensor_name() const = 0;
    virtual SensorType get_sensor_type() const = 0;

    virtual double get_set_ccd_temperature() const = 0;
    virtual void set_set_ccd_temperature(double temperature) = 0;

    virtual int get_start_x() const = 0;
    virtual void set_start_x(int start_x) = 0;

    virtual int get_start_y() const = 0;
    virtual void set_start_y(int start_y) = 0;

    virtual double get_sub_exposure_duration() const = 0;
    virtual void set_sub_exposure_duration(double duration) = 0;

    // Camera-specific methods

    virtual void abort_exposure() = 0;

    /**
     * @brief Pulse guide the camera.
     *
     * @param direction Direction (0=North, 1=South, 2=East, 3=West)
     * @param duration Duration in milliseconds
     */
    virtual void pulse_guide(int direction, int duration) = 0;

    /**
     * @brief Start an exposure.
     *
     * @param duration Exposure duration in seconds
     * @param light Whether this is a light frame (true) or dark frame (false)
     */
    virtual void start_exposure(double duration, bool light) = 0;

    /**
     * @brief Stop the current exposure.
     */
    virtual void stop_exposure() = 0;
};

} // namespace alpacacore

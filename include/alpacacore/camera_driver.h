// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://github.com/open-astro/AlpacaCore/blob/main/LICENSE
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

#pragma once

#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/error_handling.h>
#include <cstdint>
#include <vector>
#include <chrono>
#include <memory>

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
 * @brief Camera image array structure.
 */
struct ImageArray {
    std::vector<std::uint8_t> data;
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

    // Camera-specific properties

    /**
     * @brief Get the camera's X pixel size in microns.
     */
    virtual double get_pixel_size_x() const = 0;

    /**
     * @brief Get the camera's Y pixel size in microns.
     */
    virtual double get_pixel_size_y() const = 0;

    /**
     * @brief Get the camera's maximum binning X.
     */
    virtual int get_max_bin_x() const = 0;

    /**
     * @brief Get the camera's maximum binning Y.
     */
    virtual int get_max_bin_y() const = 0;

    /**
     * @brief Get the camera's current binning X.
     */
    virtual int get_bin_x() const = 0;

    /**
     * @brief Set the camera's binning X.
     */
    virtual void set_bin_x(int bin_x) = 0;

    /**
     * @brief Get the camera's current binning Y.
     */
    virtual int get_bin_y() const = 0;

    /**
     * @brief Set the camera's binning Y.
     */
    virtual void set_bin_y(int bin_y) = 0;

    /**
     * @brief Get the camera's sensor type.
     */
    virtual SensorType get_sensor_type() const = 0;

    /**
     * @brief Get the camera's sensor name.
     */
    virtual std::string get_sensor_name() const = 0;

    /**
     * @brief Get the camera's maximum ADU value.
     */
    virtual int get_max_adu() const = 0;

    /**
     * @brief Get whether the camera can abort exposure.
     */
    virtual bool get_can_abort_exposure() const = 0;

    /**
     * @brief Get whether the camera can stop exposure.
     */
    virtual bool get_can_async_readout() const = 0;

    /**
     * @brief Get whether the camera has a shutter.
     */
    virtual bool get_has_shutter() const = 0;

    /**
     * @brief Get the camera's image array.
     */
    virtual ImageArray get_image_array() const = 0;

    /**
     * @brief Get whether the camera is cooling.
     */
    virtual bool get_cooler_on() const = 0;

    /**
     * @brief Set whether the camera is cooling.
     */
    virtual void set_cooler_on(bool cooler_on) = 0;

    /**
     * @brief Get the camera's cooler power.
     */
    virtual double get_cooler_power() const = 0;

    /**
     * @brief Get the camera's CCD temperature.
     */
    virtual double get_ccd_temperature() const = 0;

    /**
     * @brief Get the camera's set CCD temperature.
     */
    virtual double get_set_ccd_temperature() const = 0;

    /**
     * @brief Set the camera's CCD temperature.
     */
    virtual void set_set_ccd_temperature(double temperature) = 0;

    /**
     * @brief Get the camera's heat sink temperature.
     */
    virtual double get_heat_sink_temperature() const = 0;

    /**
     * @brief Get the camera's exposure time in seconds.
     */
    virtual double get_exposure_time() const = 0;

    /**
     * @brief Set the camera's exposure time in seconds.
     */
    virtual void set_exposure_time(double seconds) = 0;

    /**
     * @brief Get whether the camera is exposing.
     */
    virtual bool get_image_ready() const = 0;

    /**
     * @brief Get the camera's image array variant.
     */
    virtual std::string get_image_array_variant() const = 0;

    /**
     * @brief Get the camera's last exposure duration in seconds.
     */
    virtual double get_last_exposure_duration() const = 0;

    /**
     * @brief Get the camera's last exposure start time.
     */
    virtual std::chrono::system_clock::time_point get_last_exposure_start_time() const = 0;

    /**
     * @brief Get the camera's maximum exposure time in seconds.
     */
    virtual double get_max_exposure() const = 0;

    /**
     * @brief Get the camera's minimum exposure time in seconds.
     */
    virtual double get_min_exposure() const = 0;

    /**
     * @brief Get the camera's number of X pixels.
     */
    virtual int get_num_x() const = 0;

    /**
     * @brief Get the camera's number of Y pixels.
     */
    virtual int get_num_y() const = 0;

    /**
     * @brief Get the camera's subframe X start position.
     */
    virtual int get_start_x() const = 0;

    /**
     * @brief Set the camera's subframe X start position.
     */
    virtual void set_start_x(int start_x) = 0;

    /**
     * @brief Get the camera's subframe Y start position.
     */
    virtual int get_start_y() const = 0;

    /**
     * @brief Set the camera's subframe Y start position.
     */
    virtual void set_start_y(int start_y) = 0;

    /**
     * @brief Set the camera's subframe width.
     */
    virtual void set_num_x(int num_x) = 0;

    /**
     * @brief Set the camera's subframe height.
     */
    virtual void set_num_y(int num_y) = 0;

    // Camera-specific methods

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

    /**
     * @brief Abort the current exposure.
     */
    virtual void abort_exposure() = 0;
};

} // namespace alpacacore


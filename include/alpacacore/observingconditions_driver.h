// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://www.mongodb.com/licensing/server-side-public-license
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

#pragma once

#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/error_handling.h>
#include <chrono>
#include <vector>
#include <string>

namespace alpacacore {

/**
 * @brief Pure virtual interface for Alpaca ObservingConditions drivers.
 *
 * Follows ASCOM Alpaca ObservingConditions API specification.
 * All observing conditions drivers must implement this interface.
 */
class ObservingConditionsDriver : public AlpacaDriver {
public:
    virtual ~ObservingConditionsDriver() = default;

    // ObservingConditions-specific properties

    /**
     * @brief Get the average period in seconds.
     */
    virtual double get_average_period() const = 0;

    /**
     * @brief Set the average period in seconds.
     */
    virtual void set_average_period(double period) = 0;

    /**
     * @brief Get cloud cover percentage (0-100).
     */
    virtual double get_cloud_cover() const = 0;

    /**
     * @brief Get dew point in degrees Celsius.
     */
    virtual double get_dew_point() const = 0;

    /**
     * @brief Get humidity percentage (0-100).
     */
    virtual double get_humidity() const = 0;

    /**
     * @brief Get pressure in hectopascals.
     */
    virtual double get_pressure() const = 0;

    /**
     * @brief Get rain rate in mm/hour.
     */
    virtual double get_rain_rate() const = 0;

    /**
     * @brief Get sky brightness in Lux.
     */
    virtual double get_sky_brightness() const = 0;

    /**
     * @brief Get sky quality in magnitudes per square arcsecond.
     */
    virtual double get_sky_quality() const = 0;

    /**
     * @brief Get sky temperature in degrees Celsius.
     */
    virtual double get_sky_temperature() const = 0;

    /**
     * @brief Get seeing in arcseconds.
     */
    virtual double get_seeing() const = 0;

    /**
     * @brief Get star FWHM in arcseconds.
     */
    virtual double get_star_fwhm() const = 0;

    /**
     * @brief Get temperature in degrees Celsius.
     */
    virtual double get_temperature() const = 0;

    /**
     * @brief Get wind direction in degrees (0-360).
     */
    virtual double get_wind_direction() const = 0;

    /**
     * @brief Get wind gust in meters per second.
     */
    virtual double get_wind_gust() const = 0;

    /**
     * @brief Get wind speed in meters per second.
     */
    virtual double get_wind_speed() const = 0;

    /**
     * @brief Get the time since the last sensor update.
     */
    virtual double get_time_since_last_update(std::string_view property_name) const = 0;

    /**
     * @brief Get the sensor description.
     */
    virtual std::string get_sensor_description(std::string_view property_name) const = 0;

    /**
     * @brief Refresh the sensor values.
     */
    virtual void refresh() = 0;
};

} // namespace alpacacore


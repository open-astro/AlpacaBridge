// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

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

    // Platform 7 operational state (IObservingConditionsV2): the sensor
    // properties (each omitted if that sensor is not implemented) plus a
    // TimeStamp. Inline so the vtable stays weak; values come from the same
    // getters as the GET endpoints.
    std::vector<DeviceState> get_device_state() const override final {
        std::vector<DeviceState> state;
        auto add = [&state](const char* name, auto getter) {
            try {
                state.push_back({name, DeviceStateValue{getter()}});
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                // Sensor not implemented -- or an unwrapped vendor error -- so omit per the DeviceState contract.
            }
        };
        add("CloudCover", [this] { return get_cloud_cover(); });
        add("DewPoint", [this] { return get_dew_point(); });
        add("Humidity", [this] { return get_humidity(); });
        add("Pressure", [this] { return get_pressure(); });
        add("RainRate", [this] { return get_rain_rate(); });
        add("SkyBrightness", [this] { return get_sky_brightness(); });
        add("SkyQuality", [this] { return get_sky_quality(); });
        add("SkyTemperature", [this] { return get_sky_temperature(); });
        add("StarFWHM", [this] { return get_star_fwhm(); });
        add("Temperature", [this] { return get_temperature(); });
        add("WindDirection", [this] { return get_wind_direction(); });
        add("WindGust", [this] { return get_wind_gust(); });
        add("WindSpeed", [this] { return get_wind_speed(); });
        state.push_back({"TimeStamp", device_state_timestamp()});
        return state;
    }

    // ObservingConditions-specific properties

    /**
     * @brief Get the average period in hours.
     */
    virtual double get_average_period() const = 0;

    /**
     * @brief Set the average period in hours.
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

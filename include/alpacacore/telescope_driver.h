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

namespace alpacacore {

/**
 * @brief Telescope alignment mode.
 */
enum class AlignmentMode {
    AltAz,
    Polar,
    GermanPolar
};

/**
 * @brief Telescope guide rate.
 */
struct GuideRate {
    double ra{};
    double dec{};
};

/**
 * @brief Pure virtual interface for Alpaca Telescope drivers.
 *
 * Follows ASCOM Alpaca Telescope API specification.
 * All telescope drivers must implement this interface.
 */
class TelescopeDriver : public AlpacaDriver {
public:
    virtual ~TelescopeDriver() = default;

    // Telescope-specific properties

    /**
     * @brief Get the telescope's alignment mode.
     */
    virtual AlignmentMode get_alignment_mode() const = 0;

    /**
     * @brief Get the telescope's altitude in degrees.
     */
    virtual double get_altitude() const = 0;

    /**
     * @brief Get the telescope's aperture diameter in meters.
     */
    virtual double get_aperture_diameter() const = 0;

    /**
     * @brief Get the telescope's aperture area in square meters.
     */
    virtual double get_aperture_area() const = 0;

    /**
     * @brief Get whether the telescope is at home.
     */
    virtual bool get_at_home() const = 0;

    /**
     * @brief Get whether the telescope is parked.
     */
    virtual bool get_at_park() const = 0;

    /**
     * @brief Get the telescope's azimuth in degrees.
     */
    virtual double get_azimuth() const = 0;

    /**
     * @brief Get whether the telescope can find home.
     */
    virtual bool get_can_find_home() const = 0;

    /**
     * @brief Get whether the telescope can park.
     */
    virtual bool get_can_park() const = 0;

    /**
     * @brief Get whether the telescope can pulse guide.
     */
    virtual bool get_can_pulse_guide() const = 0;

    /**
     * @brief Get whether the telescope can set declination rate.
     */
    virtual bool get_can_set_declination_rate() const = 0;

    /**
     * @brief Get whether the telescope can set guide rates.
     */
    virtual bool get_can_set_guide_rates() const = 0;

    /**
     * @brief Get whether the telescope can set park position.
     */
    virtual bool get_can_set_park() const = 0;

    /**
     * @brief Get whether the telescope can set pier side.
     */
    virtual bool get_can_set_pier_side() const = 0;

    /**
     * @brief Get whether the telescope can set right ascension rate.
     */
    virtual bool get_can_set_right_ascension_rate() const = 0;

    /**
     * @brief Get whether the telescope can set tracking.
     */
    virtual bool get_can_set_tracking() const = 0;

    /**
     * @brief Get whether the telescope can slew.
     */
    virtual bool get_can_slew() const = 0;

    /**
     * @brief Get whether the telescope can slew asynchronously.
     */
    virtual bool get_can_slew_async() const = 0;

    /**
     * @brief Get whether the telescope can sync.
     */
    virtual bool get_can_sync() const = 0;

    /**
     * @brief Get whether the telescope can unpark.
     */
    virtual bool get_can_unpark() const = 0;

    /**
     * @brief Get the telescope's declination in degrees.
     */
    virtual double get_declination() const = 0;

    /**
     * @brief Get the telescope's declination rate in arcseconds per second.
     */
    virtual double get_declination_rate() const = 0;

    /**
     * @brief Set the telescope's declination rate in arcseconds per second.
     */
    virtual void set_declination_rate(double rate) = 0;

    /**
     * @brief Get whether the telescope is tracking.
     */
    virtual bool get_tracking() const = 0;

    /**
     * @brief Set whether the telescope is tracking.
     */
    virtual void set_tracking(bool tracking) = 0;

    /**
     * @brief Get the telescope's focal length in meters.
     */
    virtual double get_focal_length() const = 0;

    /**
     * @brief Get the telescope's guide rate.
     */
    virtual GuideRate get_guide_rate() const = 0;

    /**
     * @brief Set the telescope's guide rate.
     */
    virtual void set_guide_rate(const GuideRate& rate) = 0;

    /**
     * @brief Get whether the telescope is slewing.
     */
    virtual bool get_is_slewing() const = 0;

    /**
     * @brief Get the telescope's right ascension in hours.
     */
    virtual double get_right_ascension() const = 0;

    /**
     * @brief Get the telescope's right ascension rate in arcseconds per second.
     */
    virtual double get_right_ascension_rate() const = 0;

    /**
     * @brief Set the telescope's right ascension rate in arcseconds per second.
     */
    virtual void set_right_ascension_rate(double rate) = 0;

    /**
     * @brief Get the telescope's side of pier.
     */
    virtual int get_side_of_pier() const = 0;

    /**
     * @brief Set the telescope's side of pier.
     */
    virtual void set_side_of_pier(int side) = 0;

    /**
     * @brief Get the telescope's sidereal time in hours.
     */
    virtual double get_sidereal_time() const = 0;

    /**
     * @brief Get the telescope's site elevation in meters.
     */
    virtual double get_site_elevation() const = 0;

    /**
     * @brief Set the telescope's site elevation in meters.
     */
    virtual void set_site_elevation(double elevation) = 0;

    /**
     * @brief Get the telescope's site latitude in degrees.
     */
    virtual double get_site_latitude() const = 0;

    /**
     * @brief Set the telescope's site latitude in degrees.
     */
    virtual void set_site_latitude(double latitude) = 0;

    /**
     * @brief Get the telescope's site longitude in degrees.
     */
    virtual double get_site_longitude() const = 0;

    /**
     * @brief Set the telescope's site longitude in degrees.
     */
    virtual void set_site_longitude(double longitude) = 0;

    /**
     * @brief Get the telescope's slewing state.
     */
    virtual bool get_slewing() const = 0;

    /**
     * @brief Get the telescope's target declination in degrees.
     */
    virtual double get_target_declination() const = 0;

    /**
     * @brief Set the telescope's target declination in degrees.
     */
    virtual void set_target_declination(double dec) = 0;

    /**
     * @brief Get the telescope's target right ascension in hours.
     */
    virtual double get_target_right_ascension() const = 0;

    /**
     * @brief Set the telescope's target right ascension in hours.
     */
    virtual void set_target_right_ascension(double ra) = 0;

    /**
     * @brief Get the telescope's tracking rate.
     */
    virtual double get_tracking_rate() const = 0;

    /**
     * @brief Set the telescope's tracking rate.
     */
    virtual void set_tracking_rate(double rate) = 0;

    /**
     * @brief Get the telescope's UTC date/time.
     */
    virtual std::chrono::system_clock::time_point get_utc_date() const = 0;

    /**
     * @brief Set the telescope's UTC date/time.
     */
    virtual void set_utc_date(std::chrono::system_clock::time_point utc) = 0;

    // Telescope-specific methods

    /**
     * @brief Move the telescope to the home position.
     */
    virtual void find_home() = 0;

    /**
     * @brief Move the telescope to the park position.
     */
    virtual void park() = 0;

    /**
     * @brief Pulse guide the telescope.
     *
     * @param direction Direction (0=North, 1=South, 2=East, 3=West)
     * @param duration Duration in milliseconds
     */
    virtual void pulse_guide(int direction, int duration) = 0;

    /**
     * @brief Set the telescope's park position.
     */
    virtual void set_park() = 0;

    /**
     * @brief Slew the telescope to the target coordinates.
     */
    virtual void slew_to_coordinates() = 0;

    /**
     * @brief Slew the telescope to the target coordinates asynchronously.
     */
    virtual void slew_to_coordinates_async() = 0;

    /**
     * @brief Slew the telescope to the target.
     */
    virtual void slew_to_target() = 0;

    /**
     * @brief Slew the telescope to the target asynchronously.
     */
    virtual void slew_to_target_async() = 0;

    /**
     * @brief Sync the telescope to the target coordinates.
     */
    virtual void sync_to_coordinates(double ra, double dec) = 0;

    /**
     * @brief Sync the telescope to the target.
     */
    virtual void sync_to_target() = 0;

    /**
     * @brief Unpark the telescope.
     */
    virtual void unpark() = 0;
};

} // namespace alpacacore


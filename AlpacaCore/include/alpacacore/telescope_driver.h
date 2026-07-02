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
#include <cstdint>
#include <utility>
#include <vector>

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
 * @brief Equatorial coordinate system.
 */
enum class EquatorialSystem {
    Other = 0,
    Topocentric = 1,
    J2000 = 2,
    J2050 = 3,
    B1950 = 4
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

    // Platform 7 operational state (ITelescopeV4): built from the individual
    // property getters (omit-on-throw keeps DeviceState consistent with the GET
    // endpoints) plus a TimeStamp. Inline so the vtable stays weak and the
    // per-vendor static libraries link without a base-library ordering
    // dependency. UTCDate is intentionally omitted to avoid format drift versus
    // the /utcdate endpoint; it is optional ("if known").
    std::vector<DeviceState> get_device_state() const override final {
        std::vector<DeviceState> state;
        auto add = [&state](const char* name, auto getter) {
            try {
                state.push_back({name, DeviceStateValue{getter()}});
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                // Not currently known -- or an unwrapped vendor error -- so omit per the DeviceState contract.
            }
        };
        add("Altitude", [this] { return get_altitude(); });
        add("AtHome", [this] { return get_at_home(); });
        add("AtPark", [this] { return get_at_park(); });
        add("Azimuth", [this] { return get_azimuth(); });
        add("Declination", [this] { return get_declination(); });
        add("IsPulseGuiding", [this] { return get_is_pulse_guiding(); });
        add("RightAscension", [this] { return get_right_ascension(); });
        add("SideOfPier", [this] { return static_cast<std::int32_t>(get_side_of_pier()); });
        add("SiderealTime", [this] { return get_sidereal_time(); });
        add("Slewing", [this] { return get_slewing(); });
        add("Tracking", [this] { return get_tracking(); });
        state.push_back({"TimeStamp", device_state_timestamp()});
        return state;
    }

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
     * @brief Set the telescope's aperture diameter in meters.
     */
    virtual void set_aperture_diameter(double meters) = 0;

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
     * @brief Get whether a pulse guide command is currently active.
     */
    virtual bool get_is_pulse_guiding() const = 0;

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
     * @brief Get whether the telescope can slew to Alt/Az coordinates.
     */
    virtual bool get_can_slew_alt_az() const = 0;

    /**
     * @brief Get whether the telescope can slew to Alt/Az coordinates asynchronously.
     */
    virtual bool get_can_slew_alt_az_async() const = 0;

    /**
     * @brief Get whether the telescope can sync to Alt/Az coordinates.
     */
    virtual bool get_can_sync_alt_az() const = 0;

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
     * @brief Set the telescope's focal length in meters.
     */
    virtual void set_focal_length(double meters) = 0;

    /**
     * @brief Get the telescope's guide rate.
     */
    virtual GuideRate get_guide_rate() const = 0;

    /**
     * @brief Set the telescope's guide rate.
     */
    virtual void set_guide_rate(const GuideRate& rate) = 0;

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
     * @brief Get the destination side of pier for the specified target coordinates.
     */
    virtual int get_destination_side_of_pier(double ra, double dec) const = 0;

    /**
     * @brief Get the equatorial coordinate system.
     */
    virtual EquatorialSystem get_equatorial_system() const = 0;

    /**
     * @brief Get whether atmospheric refraction is applied.
     */
    virtual bool get_does_refraction() const = 0;

    /**
     * @brief Set whether atmospheric refraction is applied.
     */
    virtual void set_does_refraction(bool does_refraction) = 0;

    /**
     * @brief Get the slew settle time in seconds.
     */
    virtual int get_slew_settle_time() const = 0;

    /**
     * @brief Set the slew settle time in seconds.
     */
    virtual void set_slew_settle_time(int seconds) = 0;

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
    virtual int get_tracking_rate() const = 0;

    /**
     * @brief Set the telescope's tracking rate.
     */
    virtual void set_tracking_rate(int rate) = 0;

    /**
     * @brief Get the collection of supported tracking rates (DriveRates).
     * 
     * Returns a vector of integers representing supported DriveRates:
     * 0 = driveSidereal, 1 = driveLunar, 2 = driveSolar, 3 = driveKing
     */
    virtual std::vector<int> get_tracking_rates() const = 0;

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
    virtual void slew_to_coordinates(double ra, double dec) = 0;

    /**
     * @brief Slew the telescope to the target coordinates asynchronously.
     */
    virtual void slew_to_coordinates_async(double ra, double dec) = 0;

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

    /**
     * @brief Check if the telescope can move the specified axis.
     *
     * @param axis Axis number (0=Primary, 1=Secondary, 2=Tertiary)
     * @return true if the axis can be moved, false otherwise
     */
    virtual bool get_can_move_axis(int axis) const = 0;

    /**
     * @brief Move the specified axis at the given rate.
     *
     * @param axis Axis number (0=Primary, 1=Secondary, 2=Tertiary)
     * @param rate Rate in degrees per second (positive or negative)
     */
    virtual void move_axis(int axis, double rate) = 0;

    /**
     * @brief Get the allowed axis rate range.
     *
     * @param axis Axis number (0=Primary, 1=Secondary, 2=Tertiary)
     * @return Pair of (min_rate, max_rate) in degrees per second.
     */
    virtual std::pair<double, double> get_axis_rate_range(int axis) const = 0;
    /**
     * @brief Get the allowed axis rate ranges.
     *
     * Use multiple ranges to represent discrete speeds.
     *
     * @param axis Axis number (0=Primary, 1=Secondary, 2=Tertiary)
     * @return Vector of (min_rate, max_rate) ranges in degrees per second.
     */
    virtual std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const {
        return {get_axis_rate_range(axis)};
    }

    /**
     * @brief Abort any current slew operation.
     */
    virtual void abort_slew() = 0;

    /**
     * @brief Slew the telescope to the given Alt/Az coordinates.
     *
     * @param altitude Altitude in degrees
     * @param azimuth Azimuth in degrees
     */
    virtual void slew_to_alt_az(double altitude, double azimuth) = 0;

    /**
     * @brief Slew the telescope to the given Alt/Az coordinates asynchronously.
     *
     * @param altitude Altitude in degrees
     * @param azimuth Azimuth in degrees
     */
    virtual void slew_to_alt_az_async(double altitude, double azimuth) = 0;

    /**
     * @brief Sync the telescope to the given Alt/Az coordinates.
     *
     * @param altitude Altitude in degrees
     * @param azimuth Azimuth in degrees
     */
    virtual void sync_to_alt_az(double altitude, double azimuth) = 0;
};

} // namespace alpacacore

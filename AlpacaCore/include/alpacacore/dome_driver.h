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

namespace alpacacore {

/**
 * @brief Pure virtual interface for Alpaca Dome drivers.
 *
 * Follows ASCOM Alpaca Dome API specification.
 * All dome drivers must implement this interface.
 */
class DomeDriver : public AlpacaDriver {
public:
    virtual ~DomeDriver() = default;

    // Dome-specific properties

    /**
     * @brief Get the altitude in degrees.
     */
    virtual double get_altitude() const = 0;

    /**
     * @brief Get whether the dome can find home.
     */
    virtual bool get_can_find_home() const = 0;

    /**
     * @brief Get whether the dome can park.
     */
    virtual bool get_can_park() const = 0;

    /**
     * @brief Get whether the dome can set altitude.
     */
    virtual bool get_can_set_altitude() const = 0;

    /**
     * @brief Get whether the dome can set azimuth.
     */
    virtual bool get_can_set_azimuth() const = 0;

    /**
     * @brief Get whether the dome can set park.
     */
    virtual bool get_can_set_park() const = 0;

    /**
     * @brief Get whether the dome can set shutter.
     */
    virtual bool get_can_set_shutter() const = 0;

    /**
     * @brief Get whether the dome can slave.
     */
    virtual bool get_can_slave() const = 0;

    /**
     * @brief Get whether the dome can slew.
     */
    virtual bool get_can_slew() const = 0;

    /**
     * @brief Get whether the dome can sync azimuth.
     */
    virtual bool get_can_sync_azimuth() const = 0;

    /**
     * @brief Get the azimuth in degrees.
     */
    virtual double get_azimuth() const = 0;

    /**
     * @brief Get whether the dome is at home.
     */
    virtual bool get_at_home() const = 0;

    /**
     * @brief Get whether the dome is at park.
     */
    virtual bool get_at_park() const = 0;

    /**
     * @brief Get whether the dome is slewing.
     */
    virtual bool get_slewing() const = 0;

    /**
     * @brief Get the shutter status.
     */
    virtual int get_shutter_status() const = 0;

    /**
     * @brief Get whether the dome is slaved to the telescope.
     */
    virtual bool get_slaved() const = 0;

    /**
     * @brief Set whether the dome is slaved to the telescope.
     */
    virtual void set_slaved(bool slaved) = 0;

    // Dome-specific methods

    /**
     * @brief Abort slew.
     */
    virtual void abort_slew() = 0;

    /**
     * @brief Close the shutter.
     */
    virtual void close_shutter() = 0;

    /**
     * @brief Find home position.
     */
    virtual void find_home() = 0;

    /**
     * @brief Open the shutter.
     */
    virtual void open_shutter() = 0;

    /**
     * @brief Park the dome.
     */
    virtual void park() = 0;

    /**
     * @brief Set park position.
     */
    virtual void set_park() = 0;

    /**
     * @brief Slew to azimuth.
     */
    virtual void slew_to_azimuth(double azimuth) = 0;

    /**
     * @brief Slew to altitude.
     */
    virtual void slew_to_altitude(double altitude) = 0;

    /**
     * @brief Sync to azimuth.
     */
    virtual void sync_to_azimuth(double azimuth) = 0;
};

} // namespace alpacacore

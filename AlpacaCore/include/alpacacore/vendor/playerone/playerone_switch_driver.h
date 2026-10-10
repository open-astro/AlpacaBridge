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

#include <alpacacore/switch_driver.h>
#include <alpacacore/vendor/playerone/playerone_sdk_wrapper.h>

#include <memory>
#include <vector>

namespace alpacacore::vendor::playerone {

/**
 * @brief Create a Player One thermal switch device (dew heater + radiator fan).
 *
 * Exposes the camera's POA_HEATER_POWER and POA_FAN_POWER controls as
 * multi-value switch elements (percent, typically 0-100). The switch shares
 * the camera's SDK handle via the reference-counted PlayerOneSDKWrapper, so
 * it can be connected alongside the camera device or on its own.
 *
 * @param device_number Alpaca device number
 * @param camera_index Player One SDK camera index (0-based, enumeration order)
 */
std::unique_ptr<SwitchDriver> create_playerone_switch(int device_number, int camera_index);

/**
 * @brief The SDK calls the thermal switch makes, as a driver-local seam so a
 *        test can count device reads without hardware (open-astro#294).
 *
 * Production uses the PlayerOneSDKWrapper singleton. Not a general SDK seam:
 * it lists only what this driver calls.
 */
class PlayerOneThermalSdk {
public:
    virtual std::vector<PlayerOneCameraInfo> enumerate_cameras() = 0;
    virtual void open_camera(int camera_id) = 0;
    virtual void init_camera(int camera_id) = 0;
    virtual void close_camera(int camera_id) = 0;
    virtual PlayerOneConfigCaps probe_config_caps(int camera_id) = 0;
    virtual int get_heater_power_percent(int camera_id) = 0;
    virtual int get_fan_power_percent(int camera_id) = 0;
    virtual void set_heater_power_percent(int camera_id, int percent) = 0;
    virtual void set_fan_power_percent(int camera_id, int percent) = 0;

protected:
    ~PlayerOneThermalSdk() = default;
};

/// Test overload: the driver keeps a reference, `sdk` must outlive it.
std::unique_ptr<SwitchDriver> create_playerone_switch(int device_number, int camera_index, PlayerOneThermalSdk& sdk);

}  // namespace alpacacore::vendor::playerone

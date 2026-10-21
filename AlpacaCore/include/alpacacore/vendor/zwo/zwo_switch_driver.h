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
#include <alpacacore/util/thermal_camera_choice.h>
#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>

#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

/**
 * @brief Create a ZWO dew heater switch device using a camera ID.
 *
 * @param device_number Alpaca device number
 * @param camera_id ZWO SDK camera ID
 */
std::unique_ptr<SwitchDriver> create_zwo_dew_heater_switch(int device_number, int camera_id);

/**
 * @brief Create a ZWO dew heater switch device using a camera index.
 *
 * @param device_number Alpaca device number
 * @param camera_index ZWO SDK camera index (0-based), or util::kAutoThermalCamera
 *        for the one attached camera that has an anti-dew heater
 * @param camera_serial When not empty, the switch binds only the camera with
 *        this serial and the index is ignored, so a USB reorder cannot move it
 */
std::unique_ptr<SwitchDriver> create_zwo_dew_heater_switch_by_index(int device_number, int camera_index,
                                                                    const std::string& camera_serial = {});

/**
 * @brief Every attached ZWO camera for a dew heater's camera picker: SDK slot,
 * model, serial, and whether it has an anti-dew heater.
 *
 * Opens each camera through the ref-counted SDK wrapper (a camera already open
 * in this process stays open) because ASIGetControlCaps and
 * ASIGetSerialNumber need an open camera.
 */
std::vector<util::ThermalCameraCandidate> list_zwo_dew_heater_cameras();

/**
 * @brief The SDK calls the dew heater switch makes, as a driver-local seam so a
 *        test can count device reads without hardware (open-astro#294).
 *
 * Production uses the ZWOSDKWrapper singleton. Not a general SDK seam: it
 * lists only what this driver calls.
 */
class ZWODewHeaterSdk {
public:
    virtual std::vector<ZWOCameraInfo> enumerate_cameras() = 0;
    /// The same list, also reporting the slots the SDK could not read so a
    /// refusal can name them; a seam with none keeps this default.
    virtual std::vector<ZWOCameraInfo> enumerate_cameras_reporting_unreadable(
        std::vector<ZwoUnreadableSlot>* /*unreadable*/) {
        return enumerate_cameras();
    }
    virtual bool get_camera_info_by_id(int camera_id, ZWOCameraInfo& info) = 0;
    virtual void open_camera(int camera_id) = 0;
    virtual void init_camera(int camera_id) = 0;
    virtual void close_camera(int camera_id) = 0;
    virtual std::vector<ZWOControlCaps> get_control_caps(int camera_id) = 0;
    virtual bool get_control_value(int camera_id, ZWOControlType type, long& value, bool& is_auto) = 0;
    virtual void set_control_value(int camera_id, ZWOControlType type, long value, bool is_auto) = 0;
    virtual std::string get_serial_number(int camera_id) = 0;

protected:
    ~ZWODewHeaterSdk() = default;
};

/// Test overload: the driver keeps a reference, `sdk` must outlive it.
std::unique_ptr<SwitchDriver> create_zwo_dew_heater_switch(int device_number, int camera_id, ZWODewHeaterSdk& sdk);

/// Test overload binding by SDK slot index (or Auto, or `camera_serial`, as
/// above); `sdk` must outlive the driver.
std::unique_ptr<SwitchDriver> create_zwo_dew_heater_switch_by_index(int device_number, int camera_index,
                                                                    ZWODewHeaterSdk& sdk,
                                                                    const std::string& camera_serial = {});

} // namespace alpacacore::vendor::zwo

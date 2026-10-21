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

// The Player One factories (camera, Phoenix filter wheel, thermal switch).
// Each create_playerone_*() is hardware-free (the SDK is touched at connect).
// This file (unlike playerone_schema.cpp) is compiled only under
// ALPACACORE_ENABLE_PLAYERONE, so the vendor headers here are fine.

#include <alpacacore/vendor/playerone/playerone_camera_driver.h>
#include <alpacacore/vendor/playerone/playerone_filterwheel_driver.h>
#include <alpacacore/vendor/playerone/playerone_switch_driver.h>

#include <algorithm>

#include "../../catalog/builtin_descriptors.h"
#include "../ioptron/ioptron_camera_fields.h"
#include "playerone_fields.h"

namespace alpacacore::catalog {

void register_playerone_factory(DeviceCatalog& catalog) {
    Factory camera;
    camera.key = DeviceKey{"playerone", DeviceType::Camera};
    camera.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        return vendor::playerone::create_playerone_camera(device_number,
                                                          static_cast<int>(config.get(kPlayerOneCameraIndex)));
    };
    catalog.add(std::move(camera));

    Factory wheel;
    wheel.key = DeviceKey{"playerone", DeviceType::FilterWheel};
    wheel.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        auto driver = vendor::playerone::create_playerone_filterwheel(
            device_number, static_cast<int>(config.get(kPlayerOneFilterwheelIndex)));
        if (auto names = config.find(kPlayerOneFilterNames)) driver->set_names(*names);
        return driver;
    };
    catalog.add(std::move(wheel));

    Factory sw;
    sw.key = DeviceKey{"playerone", DeviceType::Switch};
    sw.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        // The field's range already refuses below -1; clamp as the ZWO and ToupTek factories do.
        return vendor::playerone::create_playerone_switch(
            device_number,
            std::max(static_cast<int>(config.get(kPlayerOneSwitchCameraIndex)), util::kAutoThermalCamera),
            config.get(kPlayerOneSwitchSerial));
    };
    catalog.add(std::move(sw));

    catalog.add_thermal_camera_lister("playerone", [] { return vendor::playerone::list_playerone_thermal_cameras(); });
}

// The iOptron iCAM cameras (e.g. iCAM178M) are rebadged Player One cameras (USB
// VID a0a0; the Player One SDK enumerates them by name), so the ioptron/camera
// descriptor builds the Player One driver. It sits here, not in ioptron_catalog.cpp,
// because it needs only the Player One build, as the router arm it replaces did.
void register_ioptron_icam_factory(DeviceCatalog& catalog) {
    Factory camera;
    camera.key = DeviceKey{"ioptron", DeviceType::Camera};
    camera.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        return vendor::playerone::create_playerone_camera(device_number,
                                                          static_cast<int>(config.get(kIoptronCameraIndex)));
    };
    catalog.add(std::move(camera));
}

}  // namespace alpacacore::catalog

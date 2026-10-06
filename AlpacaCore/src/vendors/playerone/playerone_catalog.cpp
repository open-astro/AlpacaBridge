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

#include "../../catalog/builtin_descriptors.h"
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
        return vendor::playerone::create_playerone_switch(device_number,
                                                          static_cast<int>(config.get(kPlayerOneCameraIndex)));
    };
    catalog.add(std::move(sw));
}

}  // namespace alpacacore::catalog

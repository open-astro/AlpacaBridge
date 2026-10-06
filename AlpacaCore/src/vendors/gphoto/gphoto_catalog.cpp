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

// The gphoto camera factory. create_gphoto_camera() is hardware-free (the SDK
// is touched at connect). This file (unlike gphoto_schema.cpp) is compiled
// only under ALPACACORE_ENABLE_GPHOTO, so the vendor header here is fine.

#include <alpacacore/vendor/gphoto/gphoto_camera_driver.h>

#include "../../catalog/builtin_descriptors.h"
#include "gphoto_fields.h"

namespace alpacacore::catalog {

void register_gphoto_factory(DeviceCatalog& catalog) {
    Factory factory;
    factory.key = DeviceKey{"gphoto", DeviceType::Camera};
    factory.create = [](const DeviceConfig& config, int device_number) {
        return vendor::gphoto::create_gphoto_camera(device_number, static_cast<int>(config.get(kGphotoCameraIndex)));
    };
    catalog.add(std::move(factory));
}

}  // namespace alpacacore::catalog

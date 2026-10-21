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

// The Altair camera factory. Construction runs one enumeration (a failure is
// logged), nothing is opened until connect. This file
// (unlike altair_schema.cpp) is compiled only under ALPACACORE_ENABLE_ALTAIR,
// and is not in the layering gate's catalog file set (only *_schema.cpp is),
// so the vendor header here is fine.

#include <alpacacore/vendor/altair/altair_camera_driver.h>

#include "../../catalog/builtin_descriptors.h"
#include "altair_fields.h"

namespace alpacacore::catalog {

void register_altair_factory(DeviceCatalog& catalog) {
    Factory factory;
    factory.key = DeviceKey{"altair", DeviceType::Camera};
    factory.create = [](const DeviceConfig& config, int device_number) {
        const int camera_index = static_cast<int>(config.get(kAltairCameraIndex));
        return vendor::altair::create_altair_camera(device_number, camera_index);
    };
    catalog.add(std::move(factory));
}

}  // namespace alpacacore::catalog

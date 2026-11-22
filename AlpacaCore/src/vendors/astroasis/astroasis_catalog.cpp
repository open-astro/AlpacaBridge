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

// The Astroasis Oasis Focuser factory (open-astro#664): hidPath wins over
// focuserIndex when both are present, exactly like the router arm it
// replaces. create_astroasis_focuser_by_index() is hardware-free (the USB
// scan runs at connect, #659) -- never resolve_astroasis_focuser_by_index()
// directly, which would move the scan here, onto the registration path.
// This file (unlike astroasis_schema.cpp) is compiled only under
// ALPACACORE_ENABLE_ASTROASIS, and is not in the layering gate's catalog
// file set (only *_schema.cpp is), so the vendor header here is fine.

#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>
#include <alpacacore/vendor/astroasis/astroasis_protocol_wrapper.h>

#include "../../catalog/builtin_descriptors.h"
#include "astroasis_fields.h"

namespace alpacacore::catalog {

void register_astroasis_factory(DeviceCatalog& catalog) {
    Factory factory;
    factory.key = DeviceKey{"astroasis", DeviceType::Focuser};
    factory.create = [](const DeviceConfig& config, int device_number) {
        const std::string hid_path = config.get(kAstroasisHidPath);
        if (!hid_path.empty()) {
            return vendor::astroasis::create_astroasis_focuser(device_number, hid_path);
        }
        const int focuser_index = static_cast<int>(config.get(kAstroasisFocuserIndex));
        return vendor::astroasis::create_astroasis_focuser_by_index(device_number, focuser_index);
    };
    catalog.add(std::move(factory));

    // The attacheddevices listing (#2312): hid_enumerate by VID:PID, which opens no device.
    catalog.add_hid_lister(DeviceKey{"astroasis", DeviceType::Focuser}, [] {
        std::vector<AttachedHidDevice> found;
        for (const auto& port : vendor::astroasis::enumerate_astroasis_focusers()) {
            found.push_back({port.hid_path, "Astroasis Oasis Focuser"});
        }
        return found;
    });
}

}  // namespace alpacacore::catalog

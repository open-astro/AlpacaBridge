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

// The OnStep telescope factory, doing what the router arm it replaces did.
// create_onstep_telescope_auto() is hardware-free (the scan runs at connect,
// #659) -- never resolve_onstep_serial_auto() directly, which would move the
// scan here, onto the registration path. Compiled only under
// ALPACACORE_ENABLE_ONSTEP (unlike onstep_schema.cpp), so the vendor header is
// fine here.

#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>

#include <optional>
#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "onstep_fields.h"

namespace alpacacore::catalog {

void register_onstep_factory(DeviceCatalog& catalog) {
    Factory factory;
    factory.key = DeviceKey{"onstep", DeviceType::Telescope};
    factory.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        // find(), not get(): the 0.0 defaults are not a site.
        const std::optional<double> latitude = config.find(kOnStepSiteLatitude);
        const std::optional<double> longitude = config.find(kOnStepSiteLongitude);
        const std::optional<double> elevation = config.find(kOnStepSiteElevation);
        const std::optional<bool> sync_time = config.find(kOnStepSyncTimeOnConnect);

        std::unique_ptr<TelescopeDriver> telescope;
        const std::string type = config.get(kOnStepConnectionType);
        if (type.empty() || type == "auto") {
            const int mount_index = static_cast<int>(config.get(kOnStepMountIndex));
            telescope = vendor::onstep::create_onstep_telescope_auto(device_number, mount_index, latitude, longitude,
                                                                     elevation, sync_time);
        } else {
            // normalize has left "serial" here; anything else is read as
            // serial, never auto (#380).
            vendor::onstep::ConnectionInfo info;
            info.type = vendor::onstep::ConnectionType::Serial;
            info.port_path = config.get(kOnStepPortPath);
            info.baud_rate = static_cast<int>(config.get(kOnStepBaudRate));
            info.response_timeout_ms = static_cast<int>(config.get(kOnStepResponseTimeoutMs));
            telescope = vendor::onstep::create_onstep_telescope_with_site(device_number, info, latitude, longitude,
                                                                          elevation, sync_time);
        }

        if (const double aperture = config.get(kOnStepApertureDiameter); aperture > 0.0) {
            telescope->set_aperture_diameter(aperture);
        }
        if (const double focal = config.get(kOnStepFocalLength); focal > 0.0) {
            telescope->set_focal_length(focal);
        }
        if (elevation) {
            telescope->set_site_elevation(*elevation);
        }
        return telescope;
    };
    catalog.add(std::move(factory));
}

}  // namespace alpacacore::catalog

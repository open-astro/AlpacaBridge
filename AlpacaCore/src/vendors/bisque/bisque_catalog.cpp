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

// The Bisque / Paramount (TheSkyX) telescope factory, doing what the router
// arm it replaces did. Compiled only under ALPACACORE_ENABLE_BISQUE (unlike
// bisque_schema.cpp), so the vendor header is fine here.

#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>

#include <optional>

#include "../../catalog/builtin_descriptors.h"
#include "bisque_fields.h"

namespace alpacacore::catalog {

void register_bisque_factory(DeviceCatalog& catalog) {
    Factory factory;
    factory.key = DeviceKey{"bisque", DeviceType::Telescope};
    factory.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        vendor::bisque::ConnectionInfo info;
        info.host = config.get(kBisqueHost);
        info.tcp_port = static_cast<int>(config.get(kBisqueTcpPort));
        info.response_timeout_ms = static_cast<int>(config.get(kBisqueResponseTimeoutMs));

        // find(), not get(): the 0.0 default is not a site.
        const std::optional<double> elevation = config.find(kBisqueSiteElevation);
        auto telescope = vendor::bisque::create_bisque_telescope_with_site(
            device_number, info, config.find(kBisqueSiteLatitude), config.find(kBisqueSiteLongitude), elevation);

        if (const double aperture = config.get(kBisqueApertureDiameter); aperture > 0.0) {
            telescope->set_aperture_diameter(aperture);
        }
        if (const double focal = config.get(kBisqueFocalLength); focal > 0.0) {
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

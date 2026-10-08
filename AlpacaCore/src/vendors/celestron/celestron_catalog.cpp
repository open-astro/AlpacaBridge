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

// The Celestron telescope factory, doing what the router arm it replaces did.
// create_celestron_telescope_auto() is hardware-free (the scan runs at connect,
// #659) -- never resolve_celestron_serial_auto() directly, which would move the
// scan here, onto the registration path. Compiled only under
// ALPACACORE_ENABLE_CELESTRON (unlike celestron_schema.cpp), so the vendor
// header is fine here.

#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

#include <optional>
#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "celestron_fields.h"

namespace alpacacore::catalog {

void register_celestron_factory(DeviceCatalog& catalog) {
    Factory factory;
    factory.key = DeviceKey{"celestron", DeviceType::Telescope};
    factory.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        // find(), not get(): the 0.0 defaults are not a site.
        const std::optional<double> latitude = config.find(kCelestronSiteLatitude);
        const std::optional<double> longitude = config.find(kCelestronSiteLongitude);
        const std::optional<double> elevation = config.find(kCelestronSiteElevation);
        const std::optional<bool> sync_time = config.find(kCelestronSyncTimeOnConnect);

        // normalize has dropped an unknown value, so anything else is "auto".
        auto alignment = vendor::celestron::CelestronAlignmentSetting::Auto;
        const std::string mode = config.get(kCelestronAlignmentMode);
        if (mode == "altaz") {
            alignment = vendor::celestron::CelestronAlignmentSetting::AltAz;
        } else if (mode == "equatorial") {
            alignment = vendor::celestron::CelestronAlignmentSetting::Equatorial;
        }

        std::unique_ptr<TelescopeDriver> telescope;
        const std::string type = config.get(kCelestronConnectionType);
        if (type.empty() || type == "auto") {
            const int mount_index = static_cast<int>(config.get(kCelestronMountIndex));
            telescope = vendor::celestron::create_celestron_telescope_auto(device_number, mount_index, latitude,
                                                                           longitude, elevation, sync_time, alignment);
        } else {
            // normalize has left "serial" or "network" here; anything else is
            // read as serial, never auto (#380).
            vendor::celestron::ConnectionInfo info;
            if (type == "network") {
                info.type = vendor::celestron::ConnectionType::Network;
                info.host = config.get(kCelestronHost);
                info.tcp_port = static_cast<int>(config.get(kCelestronTcpPort));
            } else {
                info.type = vendor::celestron::ConnectionType::Serial;
                info.port_path = config.get(kCelestronPortPath);
                info.baud_rate = static_cast<int>(config.get(kCelestronBaudRate));
            }
            info.response_timeout_ms = static_cast<int>(config.get(kCelestronResponseTimeoutMs));
            telescope = vendor::celestron::create_celestron_telescope_with_site(
                device_number, info, latitude, longitude, elevation, sync_time, alignment);
        }

        if (const double aperture = config.get(kCelestronApertureDiameter); aperture > 0.0) {
            telescope->set_aperture_diameter(aperture);
        }
        if (const double focal = config.get(kCelestronFocalLength); focal > 0.0) {
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

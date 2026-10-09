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

// The SynScan telescope factory, doing what the router arm it replaces did.
// create_synscan_telescope_auto() is hardware-free (the scan runs at connect,
// #659) -- never resolve_synscan_serial_auto() directly, which would move the
// scan here, onto the registration path. Compiled only under
// ALPACACORE_ENABLE_SYNSCAN (unlike synscan_schema.cpp), so the vendor header
// is fine here.

#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "synscan_fields.h"

namespace alpacacore::catalog {

void register_synscan_factory(DeviceCatalog& catalog) {
    Factory factory;
    factory.key = DeviceKey{"synscan", DeviceType::Telescope};
    factory.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        // find(), not get(): the 0.0 defaults are not a site.
        const std::optional<double> latitude = config.find(kSynScanSiteLatitude);
        const std::optional<double> longitude = config.find(kSynScanSiteLongitude);
        const std::optional<double> elevation = config.find(kSynScanSiteElevation);
        const std::optional<bool> sync_time = config.find(kSynScanSyncTimeOnConnect);

        std::string version_text = config.get(kSynScanVersion);
        std::transform(version_text.begin(), version_text.end(), version_text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto version = vendor::synscan::SynScanVersion::Auto;
        if (version_text == "v3" || version_text == "3") {
            version = vendor::synscan::SynScanVersion::V3;
        } else if (version_text == "v4" || version_text == "4") {
            version = vendor::synscan::SynScanVersion::V4;
        }

        // normalize has dropped an unknown value, so anything else is "auto".
        auto alignment = vendor::synscan::SynScanAlignmentSetting::Auto;
        const std::string mode = config.get(kSynScanAlignmentMode);
        if (mode == "altaz") {
            alignment = vendor::synscan::SynScanAlignmentSetting::AltAz;
        } else if (mode == "equatorial") {
            alignment = vendor::synscan::SynScanAlignmentSetting::Equatorial;
        }

        std::unique_ptr<TelescopeDriver> telescope;
        const std::string type = config.get(kSynScanConnectionType);
        if (type.empty() || type == "auto") {
            const int mount_index = static_cast<int>(config.get(kSynScanMountIndex));
            telescope = vendor::synscan::create_synscan_telescope_auto(device_number, mount_index, version, latitude,
                                                                       longitude, elevation, sync_time, alignment);
        } else {
            // normalize has left "serial" or "network" here; anything else is
            // read as serial, never auto (#380).
            vendor::synscan::ConnectionInfo info;
            if (type == "network") {
                info.type = vendor::synscan::ConnectionType::Network;
                info.host = config.get(kSynScanHost);
                info.tcp_port = static_cast<int>(config.get(kSynScanTcpPort));
            } else {
                info.type = vendor::synscan::ConnectionType::Serial;
                info.port_path = config.get(kSynScanPortPath);
                info.baud_rate = static_cast<int>(config.get(kSynScanBaudRate));
            }
            info.response_timeout_ms = static_cast<int>(config.get(kSynScanResponseTimeoutMs));
            telescope = vendor::synscan::create_synscan_telescope_with_site(device_number, info, version, latitude,
                                                                            longitude, elevation, sync_time, alignment);
        }

        if (const double aperture = config.get(kSynScanApertureDiameter); aperture > 0.0) {
            telescope->set_aperture_diameter(aperture);
        }
        if (const double focal = config.get(kSynScanFocalLength); focal > 0.0) {
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

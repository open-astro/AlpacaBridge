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

// The Sky-Watcher direct (motor controller) telescope factory
// (open-astro#744), doing what the router arm it replaces did.
// create_skywatcher_telescope_auto() is hardware-free (the scan runs at
// connect, #659) -- never resolve_skywatcher_auto() directly, which would move
// the scan here, onto the registration path. This file (unlike
// skywatcher_schema.cpp) is compiled only under ALPACACORE_ENABLE_SKYWATCHER,
// and is not in the layering gate's catalog file set (only *_schema.cpp is),
// so the vendor header here is fine.

#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <optional>
#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "skywatcher_fields.h"

namespace alpacacore::catalog {

void register_skywatcher_factory(DeviceCatalog& catalog) {
    Factory factory;
    factory.key = DeviceKey{"skywatcher", DeviceType::Telescope};
    factory.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        // find(), not get(): the 0.0 default is not a site.
        const std::optional<double> latitude = config.find(kSkyWatcherSiteLatitude);
        const std::optional<double> longitude = config.find(kSkyWatcherSiteLongitude);
        const std::optional<double> elevation = config.find(kSkyWatcherSiteElevation);

        // #436: off unless set; find() reads an absent or null value as unset.
        util::MotionLimits limits;
        limits.min_altitude_deg = config.find(kSkyWatcherMinAltitudeDeg);
        limits.meridian_limit_minutes = config.find(kSkyWatcherMeridianLimitMinutes);

        // #274: normalize refuses a config from the API with no site, so only a
        // saved one gets here without it. Register it so it stays listed and
        // editable in the web UI; the driver refuses the connect until it is fixed.
        if (!latitude || !longitude) {
            const char* missing = (!latitude && !longitude) ? "site coordinates"
                                  : !latitude               ? "site latitude"
                                                            : "site longitude";
            ALPACA_LOG_WARN("SkyWatcherCatalog", "Persisted Sky-Watcher telescope " + std::to_string(device_number) +
                                                     " has no " + missing + " and will refuse to connect. " +
                                                     std::string(kSkyWatcherMissingSite));
        }

        // #582: normalize drops an unknown value, so anything else is "auto".
        const std::string sense_text = config.get(kSkyWatcherDecAxisSense);
        const auto dec_axis_sense = sense_text == "normal"     ? vendor::skywatcher::DecAxisSenseSetting::Normal
                                    : sense_text == "reversed" ? vendor::skywatcher::DecAxisSenseSetting::Reversed
                                                               : vendor::skywatcher::DecAxisSenseSetting::Auto;

        std::unique_ptr<TelescopeDriver> telescope;
        const std::string type = config.get(kSkyWatcherConnectionType);
        if (type.empty() || type == "auto") {
            const int mount_index = static_cast<int>(config.get(kSkyWatcherMountIndex));
            telescope = vendor::skywatcher::create_skywatcher_telescope_auto(
                device_number, mount_index, latitude, longitude, elevation, limits, dec_axis_sense);
        } else {
            // normalize has left "serial" or "network" here; anything else is
            // read as serial, never auto (#380).
            vendor::skywatcher::ConnectionInfo info;
            if (type == "network") {
                info.type = vendor::skywatcher::ConnectionType::Network;
                info.host = config.get(kSkyWatcherHost);
                info.udp_port = static_cast<int>(config.get(kSkyWatcherUdpPort));
            } else {
                info.type = vendor::skywatcher::ConnectionType::Serial;
                info.port_path = config.get(kSkyWatcherPortPath);
                info.baud_rate = static_cast<int>(config.get(kSkyWatcherBaudRate));
            }
            info.response_timeout_ms = static_cast<int>(config.get(kSkyWatcherResponseTimeoutMs));
            telescope =
                vendor::skywatcher::create_skywatcher_telescope(device_number, info, latitude, longitude, elevation, {},
                                                                limits, util::default_task_clock(), dec_axis_sense);
        }

        if (const double aperture = config.get(kSkyWatcherApertureDiameter); aperture > 0.0) {
            telescope->set_aperture_diameter(aperture);
        }
        if (const double focal = config.get(kSkyWatcherFocalLength); focal > 0.0) {
            telescope->set_focal_length(focal);
        }
        return telescope;
    };
    catalog.add(std::move(factory));
}

}  // namespace alpacacore::catalog

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

// The OnStep telescope, focuser, rotator and switch factories (the telescope's doing what the router arm it replaced
// did). create_onstep_telescope_auto() is hardware-free (the scan runs at connect, #659) -- never
// resolve_onstep_serial_auto() directly, which would move the scan here, onto the registration path. Compiled only
// under ALPACACORE_ENABLE_ONSTEP (unlike onstep_schema.cpp), so the vendor header is fine here.

#include <alpacacore/vendor/onstep/onstep_focuser_driver.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_rotator_driver.h>
#include <alpacacore/vendor/onstep/onstep_switch_driver.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "onstep_fields.h"

namespace alpacacore::catalog {

namespace {

// The endpoint of a configured (non-auto) connection, shared by both device types. normalize has left "serial" or
// "network" here; anything else is read as serial, never auto (#380). Only "auto" scans, and only the serial ports:
// a network endpoint is the configured one (#2220).
vendor::onstep::ConnectionInfo configured_connection(const DeviceConfig& config) {
    vendor::onstep::ConnectionInfo info;
    if (config.get(kOnStepConnectionType) == "network") {
        info.type = vendor::onstep::ConnectionType::Network;
        info.host = config.get(kOnStepHost);
        // Range-checked before the narrowing cast (decision 0007 rule 4): a saved port outside 1-65535
        // (normalize warned and kept it) becomes 0, which the connect refuses, never a port that wrapped.
        const std::int64_t port = config.get(kOnStepTcpPort);
        info.tcp_port = port >= kOnStepTcpPortMin && port <= kOnStepTcpPortMax ? static_cast<int>(port) : 0;
    } else {
        info.type = vendor::onstep::ConnectionType::Serial;
        info.port_path = config.get(kOnStepPortPath);
        info.baud_rate = static_cast<int>(config.get(kOnStepBaudRate));
    }
    // Clamped before the cast for the same reason; 0 or less reads as the wrapper's 5 s default.
    info.response_timeout_ms = static_cast<int>(
        std::clamp<std::int64_t>(config.get(kOnStepResponseTimeoutMs), 0, std::numeric_limits<int>::max()));
    return info;
}

}  // namespace

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
            telescope = vendor::onstep::create_onstep_telescope_with_site(device_number, configured_connection(config),
                                                                          latitude, longitude, elevation, sync_time);
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

    // The focuser joins the controller's shared link (#2223); auto-detect is the telescope's scan with the same
    // mount index, at connect time (#659).
    Factory focuser;
    focuser.key = DeviceKey{"onstep", DeviceType::Focuser};
    focuser.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        // 1-6 by the field's own range (an API value outside it is refused, a saved one reads as the default 1).
        const int focuser_number = static_cast<int>(std::clamp<std::int64_t>(config.get(kOnStepFocuserNumber), 1, 6));
        const std::string type = config.get(kOnStepConnectionType);
        if (type.empty() || type == "auto") {
            return vendor::onstep::create_onstep_focuser_auto(
                device_number, static_cast<int>(config.get(kOnStepMountIndex)), focuser_number);
        }
        return vendor::onstep::create_onstep_focuser(device_number, configured_connection(config), focuser_number);
    };
    catalog.add(std::move(focuser));

    // The rotator joins the controller's shared link too (#2221), found the same way.
    Factory rotator;
    rotator.key = DeviceKey{"onstep", DeviceType::Rotator};
    rotator.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        const std::string type = config.get(kOnStepConnectionType);
        if (type.empty() || type == "auto") {
            return vendor::onstep::create_onstep_rotator_auto(device_number,
                                                              static_cast<int>(config.get(kOnStepMountIndex)));
        }
        return vendor::onstep::create_onstep_rotator(device_number, configured_connection(config));
    };
    catalog.add(std::move(rotator));

    // The auxiliary features as one Switch on the controller's shared link (#2222), found the same way.
    Factory aux_switch;
    aux_switch.key = DeviceKey{"onstep", DeviceType::Switch};
    aux_switch.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        const std::string type = config.get(kOnStepConnectionType);
        if (type.empty() || type == "auto") {
            return vendor::onstep::create_onstep_switch_auto(device_number,
                                                             static_cast<int>(config.get(kOnStepMountIndex)));
        }
        return vendor::onstep::create_onstep_switch(device_number, configured_connection(config));
    };
    catalog.add(std::move(aux_switch));

    // The attacheddevices listing (#2312): each serial device's USB identity is the
    // token list its raw-node scan filter matches, read from the wrapper.
    catalog.add_usb_serial_identity(
        {DeviceKey{"onstep", DeviceType::Telescope}, vendor::onstep::onstep_usb_tokens(), {}});
}

}  // namespace alpacacore::catalog

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

// The iOptron factories (telescope, iMate PowerBox switch, iEAF focuser, iEFW
// filter wheel, and the iGuider guide camera under its own "iguider" key),
// doing what the router arms they replace did. Every auto-detect
// path goes through a create_*_auto / create_*_by_index factory, which is
// hardware-free (the scan runs at connect, #659) -- never a resolve_* scan
// function, which would move the scan here, onto the registration path. Compiled
// only under ALPACACORE_ENABLE_IOPTRON (unlike ioptron_schema.cpp), so the vendor
// headers are fine here; the PowerBox factory is additionally under
// ALPACACORE_IOPTRON_POWERBOX, and without it the switch throws the "not built"
// text the router returned. The iCAM camera factory is in playerone_catalog.cpp:
// it needs only the Player One build, as the router arm did.

#include <alpacacore/vendor/ioptron/ioptron_filterwheel_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ieaf_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_iguider_camera_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>
#ifdef ALPACACORE_IOPTRON_POWERBOX
#include <alpacacore/vendor/ioptron/ioptron_switch_driver.h>
#endif

#include <optional>
#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "ioptron_fields.h"

namespace alpacacore::catalog {

namespace {

std::unique_ptr<AlpacaDriver> create_telescope(const DeviceConfig& config, int device_number) {
    // find(), not get(): the 0.0 defaults are not a site.
    const std::optional<double> latitude = config.find(kIoptronSiteLatitude);
    const std::optional<double> longitude = config.find(kIoptronSiteLongitude);
    const std::optional<double> elevation = config.find(kIoptronSiteElevation);
    const std::optional<bool> sync_time = config.find(kIoptronSyncTimeOnConnect);

    std::unique_ptr<TelescopeDriver> telescope;
    const std::string type = config.get(kIoptronConnectionType);
    const int mount_index = static_cast<int>(config.get(kIoptronMountIndex));
    if (type.empty() || type == "auto") {
        telescope = vendor::ioptron::create_ioptron_telescope_auto(device_number, mount_index, latitude, longitude,
                                                                   elevation, sync_time);
    } else if (type == "network" && config.get(kIoptronHost).empty()) {
        telescope = vendor::ioptron::create_ioptron_telescope_auto_network(device_number, mount_index, latitude,
                                                                           longitude, elevation, sync_time);
    } else {
        // normalize has left "serial" or "network" here; anything else is read as
        // serial, never auto (#380).
        vendor::ioptron::ConnectionInfo info;
        if (type == "network") {
            info.type = vendor::ioptron::ConnectionType::Network;
            info.host = config.get(kIoptronHost);
            info.tcp_port = static_cast<int>(config.get(kIoptronTcpPort));
        } else {
            info.type = vendor::ioptron::ConnectionType::Serial;
            info.port_path = config.get(kIoptronPortPath);
            info.baud_rate = static_cast<int>(config.get(kIoptronBaudRate));
        }
        info.response_timeout_ms = static_cast<int>(config.get(kIoptronResponseTimeoutMs));
        telescope = vendor::ioptron::create_ioptron_telescope_with_site(device_number, info, latitude, longitude,
                                                                        elevation, sync_time);
    }

    if (const double aperture = config.get(kIoptronApertureDiameter); aperture > 0.0) {
        telescope->set_aperture_diameter(aperture);
    }
    if (const double focal = config.get(kIoptronFocalLength); focal > 0.0) {
        telescope->set_focal_length(focal);
    }
    if (elevation) {
        telescope->set_site_elevation(*elevation);
    }
    return telescope;
}

std::unique_ptr<AlpacaDriver> create_switch(const DeviceConfig& config, [[maybe_unused]] int device_number) {
    if (const auto refusal = ioptron_switch_refusal(config)) {
        throw AlpacaException(*refusal, AlpacaError::InvalidValue);
    }
#ifdef ALPACACORE_IOPTRON_POWERBOX
    // iMate PowerBox: on-board DC power ports driven over local GPIO (libgpiod),
    // independent of the mount RS-232 protocol. Switch 0 is the always-on DC
    // pass-through (read-only); switches 1/2 are the controllable DC1/DC2 lines.
    auto powerbox_config = vendor::ioptron::default_imate_powerbox_config();
    powerbox_config.gpio_chip_path = config.get(kIoptronGpioChip);
    if (const auto frequency = config.find(kIoptronPwmFrequencyHz)) {
        powerbox_config.pwm_frequency_hz = static_cast<std::uint32_t>(*frequency);
    }
    // Per-port overrides apply positionally onto the fixed layout; the pass-through
    // has no GPIO line and can't be PWM, so its pwm flag is ignored.
    if (const auto overrides = config.find(kIoptronPorts)) {
        auto& ports = powerbox_config.ports;
        for (std::size_t i = 0; i < ports.size() && i < overrides->size(); ++i) {
            const DeviceConfig& record = (*overrides)[i];
            if (const auto name = record.find(kIoptronPortName)) ports[i].name = *name;
            if (ports[i].has_line) {
                if (const auto pwm = record.find(kIoptronPortPwm)) ports[i].pwm_enabled = *pwm;
            }
        }
    }
    return vendor::ioptron::create_ioptron_switch(device_number, std::move(powerbox_config));
#else
    throw AlpacaException(
        "iOptron iMate PowerBox switch not built. Rebuild on a host with "
        "libgpiod (>= 2.0) installed (e.g. apt install libgpiod-dev).",
        AlpacaError::InvalidValue);
#endif
}

std::unique_ptr<AlpacaDriver> create_focuser(const DeviceConfig& config, int device_number) {
    // iEAF / iAFS2/3 electronic focuser: USB-serial only, fixed 115200 baud.
    const std::string model = config.get(kIoptronFocuserModel);
    if (config.get(kIoptronConnectionType) == "serial") {
        const std::string port_path = config.get(kIoptronPortPath);
        if (!port_path.empty()) return vendor::ioptron::create_ieaf_focuser(device_number, port_path, model);
    }
    // "auto", unset, or serial with no port: auto-detect at connect.
    return vendor::ioptron::create_ieaf_focuser_by_index(device_number,
                                                         static_cast<int>(config.get(kIoptronFocuserIndex)), model);
}

std::unique_ptr<AlpacaDriver> create_filterwheel(const DeviceConfig& config, int device_number) {
    // Refused here as well as in normalize, so a saved entry normalize could only warn
    // about is never built (it stays dropped, as the router arm dropped it).
    if (const auto refusal = ioptron_filterwheel_refusal(config)) {
        throw AlpacaException(*refusal, AlpacaError::InvalidValue);
    }
    // iEFW-15 / iEFW-18: USB-serial only, fixed 115200 baud. The slot count comes
    // from the wheel's handshake, not from config.
    const std::string model = config.get(kIoptronFilterwheelModel);
    std::unique_ptr<FilterWheelDriver> wheel;
    if (config.get(kIoptronConnectionType) == "serial") {
        wheel = vendor::ioptron::create_iefw_filterwheel(device_number, config.get(kIoptronPortPath), model);
    } else {
        wheel = vendor::ioptron::create_iefw_filterwheel_by_index(
            device_number, static_cast<int>(config.get(kIoptronFilterwheelIndex)), model);
    }
    if (const auto names = config.find(kIoptronFilterNames)) wheel->set_names(*names);
    return wheel;
}

std::unique_ptr<AlpacaDriver> create_iguider(const DeviceConfig& config, int device_number) {
    // Refused here as well as in normalize, as for the filter wheel.
    if (const auto refusal = iguider_camera_refusal(config)) {
        throw AlpacaException(*refusal, AlpacaError::InvalidValue);
    }
    // Hardware-free: an empty path is resolved by a V4L2 scan at connect.
    return vendor::ioptron::create_iguider_camera(device_number, config.get(kIguiderDevicePath));
}

}  // namespace

void register_ioptron_factory(DeviceCatalog& catalog) {
    const struct {
        DeviceType type;
        std::unique_ptr<AlpacaDriver> (*create)(const DeviceConfig&, int);
    } factories[] = {{DeviceType::Telescope, create_telescope},
                     {DeviceType::Switch, create_switch},
                     {DeviceType::Focuser, create_focuser},
                     {DeviceType::FilterWheel, create_filterwheel}};
    for (const auto& entry : factories) {
        Factory factory;
        factory.key = DeviceKey{"ioptron", entry.type};
        factory.create = entry.create;
        catalog.add(std::move(factory));
    }
    Factory iguider;
    iguider.key = DeviceKey{"iguider", DeviceType::Camera};
    iguider.create = create_iguider;
    catalog.add(std::move(iguider));
}

}  // namespace alpacacore::catalog

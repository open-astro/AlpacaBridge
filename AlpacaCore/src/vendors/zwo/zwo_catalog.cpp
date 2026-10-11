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

// The ZWO factories, doing what the router arms they replace did. Compiled only
// under ALPACACORE_ENABLE_ZWO (unlike zwo_schema.cpp), so the vendor headers are
// fine here. Every create_* call is hardware-free (the SDK is reached at
// connect), so nothing is probed at registration. The camera stays in the router.

#include <alpacacore/vendor/zwo/zwo_asiair_plus_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>
#include <alpacacore/vendor/zwo/zwo_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include <optional>
#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "zwo_fields.h"

namespace alpacacore::catalog {

namespace {

void refuse_if(const std::optional<std::string>& refusal) {
    if (refusal) throw AlpacaException(*refusal, AlpacaError::InvalidValue);
}

std::unique_ptr<AlpacaDriver> create_telescope(const DeviceConfig& config, int device_number) {
    vendor::zwo::ConnectionInfo info;
    // normalize has left "auto", "serial" or "network"; an absent value (no
    // normalize ran) is "auto", and anything else is read as serial, never auto (#380).
    const std::string type = config.get(kZwoConnectionType);
    if (type == "auto") {
        info.type = vendor::zwo::ConnectionType::Auto;
    } else if (type == "network") {
        info.type = vendor::zwo::ConnectionType::Network;
        info.host = config.get(kZwoHost);
        info.tcp_port = static_cast<int>(config.get(kZwoTcpPort));
    } else {
        info.type = vendor::zwo::ConnectionType::Serial;
        info.port_path = config.get(kZwoPortPath);
        info.baud_rate = static_cast<int>(config.get(kZwoBaudRate));
    }
    if (const auto timeout = config.find(kZwoResponseTimeoutMs)) {
        info.response_timeout_ms = static_cast<int>(*timeout);
    }

    // find(), not get(): the 0.0 defaults are not a site.
    const std::optional<double> latitude = config.find(kZwoSiteLatitude);
    const std::optional<double> longitude = config.find(kZwoSiteLongitude);
    const std::optional<double> elevation = config.find(kZwoSiteElevation);
    const std::optional<bool> sync_time = config.find(kZwoSyncTimeOnConnect);
    auto telescope =
        vendor::zwo::create_zwo_telescope_with_site(device_number, info, latitude, longitude, elevation, sync_time);

    if (const double aperture = config.get(kZwoApertureDiameter); aperture > 0.0) {
        telescope->set_aperture_diameter(aperture);
    }
    if (const double focal = config.get(kZwoFocalLength); focal > 0.0) {
        telescope->set_focal_length(focal);
    }
    if (elevation) {
        telescope->set_site_elevation(*elevation);
    }
    return telescope;
}

std::unique_ptr<AlpacaDriver> create_switch(const DeviceConfig& config, int device_number) {
    refuse_if(zwo_switch_refusal(config));
    const std::string type = zwo_switch_type(config);

    if (type == "asiair-plus-rk3568") {
        auto plus_config = vendor::zwo::default_asiair_plus_rk3568_config();
        if (const auto hz = config.find(kZwoPwmFrequencyHz)) {
            plus_config.pwm_frequency_hz = static_cast<decltype(plus_config.pwm_frequency_hz)>(*hz);
        }
        if (const auto records = config.find(kZwoPorts); records && !records->empty()) {
            std::vector<vendor::zwo::AsiairPlusPortConfig> ports;
            ports.reserve(records->size());
            for (const DeviceConfig& record : *records) {
                vendor::zwo::AsiairPlusPortConfig port;
                port.name = record.find(kZwoPortName).value_or("Port " + std::to_string(ports.size() + 1));
                port.pwm_enabled = record.get(kZwoPortPwm);
                ports.push_back(std::move(port));
            }
            plus_config.ports = std::move(ports);
        }
        return vendor::zwo::create_zwo_asiair_plus_switch(device_number, plus_config);
    }

    // ASIAIR Pro (Pi 4) and ASIAIR Plus (Pi CM4) share the same on-board libgpiod
    // wiring, verified against live CM4 hardware: one driver and default config;
    // only the model label differs.
    if (type == "asiair" || type == "asiair-plus-picm4") {
        auto asiair_config = vendor::zwo::default_asiair_pro_config();
        if (type == "asiair-plus-picm4") {
            asiair_config.model_name = "ASIAIR Plus (Pi CM4)";
        }
        asiair_config.gpio_chip_path = config.get(kZwoGpioChip);
        if (const auto hz = config.find(kZwoPwmFrequencyHz)) {
            asiair_config.pwm_frequency_hz = static_cast<decltype(asiair_config.pwm_frequency_hz)>(*hz);
        }
        if (const auto records = config.find(kZwoPorts); records && !records->empty()) {
            std::vector<vendor::zwo::AsiairPortConfig> ports;
            ports.reserve(records->size());
            for (const DeviceConfig& record : *records) {
                vendor::zwo::AsiairPortConfig port;
                port.name = record.find(kZwoPortName).value_or("Port " + std::to_string(ports.size() + 1));
                port.gpio_line = static_cast<std::uint32_t>(record.get(kZwoPortGpio));
                port.pwm_enabled = record.get(kZwoPortPwm);
                ports.push_back(std::move(port));
            }
            asiair_config.ports = std::move(ports);
        }
        return vendor::zwo::create_zwo_asiair_switch(device_number, std::move(asiair_config));
    }

    const int camera_id = static_cast<int>(config.get(kZwoCameraId));
    if (camera_id >= 0) return vendor::zwo::create_zwo_dew_heater_switch(device_number, camera_id);
    return vendor::zwo::create_zwo_dew_heater_switch_by_index(device_number,
                                                              static_cast<int>(config.get(kZwoCameraIndex)));
}

}  // namespace

void register_zwo_factory(DeviceCatalog& catalog) {
    Factory telescope;
    telescope.key = DeviceKey{"zwo", DeviceType::Telescope};
    telescope.create = create_telescope;
    catalog.add(std::move(telescope));

    Factory wheel;
    wheel.key = DeviceKey{"zwo", DeviceType::FilterWheel};
    wheel.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        refuse_if(zwo_bound_refusal(config, kZwoFilterwheelId, kZwoFilterwheelIndex,
                                    "ZWO filter wheel requires filterwheelIndex or filterwheelId"));
        const int id = static_cast<int>(config.get(kZwoFilterwheelId));
        std::unique_ptr<FilterWheelDriver> driver =
            id >= 0 ? vendor::zwo::create_zwo_efw_filterwheel(device_number, id)
                    : vendor::zwo::create_zwo_efw_filterwheel_by_index(
                          device_number, static_cast<int>(config.get(kZwoFilterwheelIndex)));
        if (auto names = config.find(kZwoFilterNames)) driver->set_names(*names);
        return driver;
    };
    catalog.add(std::move(wheel));

    Factory focuser;
    focuser.key = DeviceKey{"zwo", DeviceType::Focuser};
    focuser.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        refuse_if(zwo_bound_refusal(config, kZwoFocuserId, kZwoFocuserIndex,
                                    "ZWO EAF focuser requires focuserIndex or focuserId"));
        const int id = static_cast<int>(config.get(kZwoFocuserId));
        if (id >= 0) return vendor::zwo::create_zwo_eaf_focuser(device_number, id);
        return vendor::zwo::create_zwo_eaf_focuser_by_index(device_number,
                                                            static_cast<int>(config.get(kZwoFocuserIndex)));
    };
    catalog.add(std::move(focuser));

    Factory rotator;
    rotator.key = DeviceKey{"zwo", DeviceType::Rotator};
    rotator.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        refuse_if(zwo_bound_refusal(config, kZwoRotatorId, kZwoRotatorIndex,
                                    "ZWO rotator requires rotatorIndex or rotatorId"));
        const int id = static_cast<int>(config.get(kZwoRotatorId));
        if (id >= 0) return vendor::zwo::create_zwo_caa_rotator(device_number, id);
        return vendor::zwo::create_zwo_caa_rotator_by_index(device_number,
                                                            static_cast<int>(config.get(kZwoRotatorIndex)));
    };
    catalog.add(std::move(rotator));

    Factory sw;
    sw.key = DeviceKey{"zwo", DeviceType::Switch};
    sw.create = create_switch;
    catalog.add(std::move(sw));
}

}  // namespace alpacacore::catalog

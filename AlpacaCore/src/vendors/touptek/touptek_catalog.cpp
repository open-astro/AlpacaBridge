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

// The ToupTek factories (camera, AAF focuser, AFW filter wheel, switch), doing
// what the router arms they replace did. Compiled only under
// ALPACACORE_ENABLE_TOUPTEK (unlike touptek_schema.cpp), so the vendor headers
// are fine; the StellaVita PowerBox factory is additionally under
// ALPACACORE_TOUPTEK_STELLAVITA, and without it a stellavita switch throws the
// "not built" text the router returned.

#include <alpacacore/vendor/touptek/touptek_camera_driver.h>
#include <alpacacore/vendor/touptek/touptek_filterwheel_driver.h>
#include <alpacacore/vendor/touptek/touptek_focuser_driver.h>
#include <alpacacore/vendor/touptek/touptek_thermal_switch_driver.h>
#ifdef ALPACACORE_TOUPTEK_STELLAVITA
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>
#endif

#include "../../catalog/builtin_descriptors.h"
#include "touptek_fields.h"

namespace alpacacore::catalog {

namespace {

std::unique_ptr<AlpacaDriver> create_switch(const DeviceConfig& config, int device_number) {
    if (auto refusal = touptek_switch_refusal(config)) throw AlpacaException(*refusal, AlpacaError::InvalidValue);
    if (config.get(kTouptekSwitchType) == "thermal") {
        return vendor::touptek::create_touptek_thermal_switch(device_number,
                                                              static_cast<int>(config.get(kTouptekThermalCameraIndex)));
    }
#ifdef ALPACACORE_TOUPTEK_STELLAVITA
    // StellaVita PowerBox: on-board 12V DC ports over local GPIO (libgpiod),
    // independent of the camera SDK. Switches 0..3 are the Port 1..4 lines.
    auto powerbox_config = vendor::touptek::default_stellavita_config();
    powerbox_config.gpio_chip_path = config.get(kTouptekGpioChip);
    powerbox_config.pwm_frequency_hz = static_cast<std::uint32_t>(config.get(kTouptekPwmFrequencyHz));
    // Per-port overrides apply positionally onto the fixed layout.
    if (auto overrides = config.find(kTouptekPorts)) {
        auto& ports = powerbox_config.ports;
        for (std::size_t i = 0; i < ports.size() && i < overrides->size(); ++i) {
            const DeviceConfig& record = (*overrides)[i];
            if (auto name = record.find(kTouptekPortName)) ports[i].name = *name;
            if (auto pwm = record.find(kTouptekPortPwm)) ports[i].pwm_enabled = *pwm;
        }
    }
    return vendor::touptek::create_touptek_switch(device_number, std::move(powerbox_config));
#else
    throw AlpacaException(
        "ToupTek StellaVita switch not built. Rebuild on a host with "
        "libgpiod (>= 2.0) installed (e.g. apt install libgpiod-dev).",
        AlpacaError::InvalidValue);
#endif
}

}  // namespace

void register_touptek_factory(DeviceCatalog& catalog) {
    Factory camera;
    camera.key = DeviceKey{"touptek", DeviceType::Camera};
    camera.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        return vendor::touptek::create_touptek_camera(device_number, static_cast<int>(config.get(kTouptekCameraIndex)));
    };
    catalog.add(std::move(camera));

    Factory focuser;
    focuser.key = DeviceKey{"touptek", DeviceType::Focuser};
    focuser.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        const std::string id = config.get(kTouptekFocuserId);
        if (!id.empty()) return vendor::touptek::create_touptek_focuser_by_id(device_number, id);
        return vendor::touptek::create_touptek_focuser_by_index(device_number,
                                                                static_cast<int>(config.get(kTouptekFocuserIndex)));
    };
    catalog.add(std::move(focuser));

    Factory wheel;
    wheel.key = DeviceKey{"touptek", DeviceType::FilterWheel};
    wheel.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        // The slot count is read from the wheel firmware at connect.
        std::unique_ptr<FilterWheelDriver> driver;
        const std::string id = config.get(kTouptekWheelId);
        if (!id.empty()) {
            driver = vendor::touptek::create_touptek_filterwheel_by_id(device_number, id);
        } else {
            driver = vendor::touptek::create_touptek_filterwheel_by_index(
                device_number, static_cast<int>(config.get(kTouptekWheelIndex)));
        }
        if (auto names = config.find(kTouptekFilterNames)) driver->set_names(*names);
        return driver;
    };
    catalog.add(std::move(wheel));

    Factory sw;
    sw.key = DeviceKey{"touptek", DeviceType::Switch};
    sw.create = create_switch;
    catalog.add(std::move(sw));
}

}  // namespace alpacacore::catalog

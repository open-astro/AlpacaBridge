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

// The QHY factories (camera, filter wheel, Q-Focuser), doing what the router
// arms they replace did. The CFW3 wheel and the Q-Focuser are built through
// create_qhy_cfw3_filterwheel_by_index / create_qhy_focuser_by_index, which are
// hardware-free (the scan runs at connect, #659) -- never a resolve_* scan
// function, which would move the scan here, onto the registration path (it
// DTR-resets every CP210x on the box). Compiled only under
// ALPACACORE_ENABLE_QHY (unlike qhy_schema.cpp), so the vendor headers are fine.

#include <alpacacore/vendor/qhy/qhy_camera_driver.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_filterwheel_driver.h>
#include <alpacacore/vendor/qhy/qhy_filterwheel_driver.h>
#include <alpacacore/vendor/qhy/qhy_focuser_driver.h>

#include "../../catalog/builtin_descriptors.h"
#include "qhy_fields.h"

namespace alpacacore::catalog {

namespace {

void throw_if_refused(const std::optional<std::string>& refusal) {
    if (refusal) throw AlpacaException(*refusal, AlpacaError::InvalidValue);
}

}  // namespace

void register_qhy_factory(DeviceCatalog& catalog) {
    Factory camera;
    camera.key = DeviceKey{"qhy", DeviceType::Camera};
    camera.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        throw_if_refused(qhy_camera_refusal(config));
        const std::string id = config.get(kQhyCameraId);
        if (!id.empty()) return vendor::qhy::create_qhy_camera(device_number, id);
        return vendor::qhy::create_qhy_camera_by_index(device_number, static_cast<int>(config.get(kQhyCameraIndex)));
    };
    catalog.add(std::move(camera));

    Factory wheel;
    wheel.key = DeviceKey{"qhy", DeviceType::FilterWheel};
    wheel.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        throw_if_refused(qhy_filterwheel_refusal(config));
        std::unique_ptr<FilterWheelDriver> driver;
        if (config.get(kQhyWheelType) == "cfw3-usb") {
            if (config.get(kQhyWheelConnectionType) == "serial") {
                driver = vendor::qhy::create_qhy_cfw3_filterwheel(device_number, config.get(kQhyWheelPortPath));
            } else {
                // "auto": the CP210x probe runs inside the wheel's connect, not here (#659).
                driver = vendor::qhy::create_qhy_cfw3_filterwheel_by_index(
                    device_number, static_cast<int>(config.get(kQhyWheelIndex)));
            }
        } else {
            const std::string id = config.get(kQhyCameraId);
            if (!id.empty()) {
                driver = vendor::qhy::create_qhy_filterwheel(device_number, id);
            } else {
                driver = vendor::qhy::create_qhy_filterwheel_by_index(device_number,
                                                                      static_cast<int>(config.get(kQhyCameraIndex)));
            }
        }
        if (auto names = config.find(kQhyFilterNames)) driver->set_names(*names);
        return driver;
    };
    catalog.add(std::move(wheel));

    Factory focuser;
    focuser.key = DeviceKey{"qhy", DeviceType::Focuser};
    focuser.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        vendor::qhy::QFocuserSettings settings;
        settings.max_step = static_cast<int>(config.get(kQhyMaxStep));
        settings.reverse = config.get(kQhyReverse);
        settings.speed = static_cast<int>(config.get(kQhySpeed));
        settings.hold_force = config.get(kQhyHoldForce);
        settings.hold_ihold = static_cast<int>(config.get(kQhyHoldIhold));
        settings.hold_irun = static_cast<int>(config.get(kQhyHoldIrun));
        settings.temperature_source = config.get(kQhyTemperatureSource);
        const std::string port_path =
            config.get(kQhyFocuserConnectionType) == "serial" ? config.get(kQhyFocuserPortPath) : std::string();
        if (!port_path.empty()) return vendor::qhy::create_qhy_focuser(device_number, port_path, settings);
        // "auto", or serial mode with no port given: auto-detect at connect.
        return vendor::qhy::create_qhy_focuser_by_index(device_number, static_cast<int>(config.get(kQhyFocuserIndex)),
                                                        settings);
    };
    catalog.add(std::move(focuser));
}

}  // namespace alpacacore::catalog

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

// The Gemini factories (focuser, flat panel, Power & Data Hub switch), doing
// what the router arms they replace did. Every auto-detect path goes through a
// create_*_by_index factory, which is hardware-free (the scan runs at connect,
// #659) -- never a resolve_* scan function, which would move the scan here, onto
// the registration path. Compiled only under ALPACACORE_ENABLE_GEMINI (unlike
// gemini_schema.cpp), so the vendor headers are fine here.

#include <alpacacore/vendor/gemini/gemini_flatpanel_driver.h>
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#include <alpacacore/vendor/gemini/gemini_pdh_switch_driver.h>

#include "../../catalog/builtin_descriptors.h"
#include "gemini_fields.h"

namespace alpacacore::catalog {

void register_gemini_factory(DeviceCatalog& catalog) {
    Factory focuser;
    focuser.key = DeviceKey{"gemini", DeviceType::Focuser};
    focuser.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        if (config.get(kGeminiConnectionType) == "serial") {
            const std::string port_path = config.get(kGeminiPortPath);
            if (!port_path.empty()) {
                return vendor::gemini::create_gemini_focuser(device_number, port_path,
                                                             static_cast<int>(config.get(kGeminiBaudRate)));
            }
        }
        // "auto", unset, or serial with no port: auto-detect at connect.
        return vendor::gemini::create_gemini_focuser_by_index(device_number,
                                                              static_cast<int>(config.get(kGeminiFocuserIndex)));
    };
    catalog.add(std::move(focuser));

    Factory panel;
    panel.key = DeviceKey{"gemini", DeviceType::CoverCalibrator};
    panel.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        const std::string model = config.get(kGeminiFlatPanelModel);
        const bool is_pro = model == "pro";
        const bool is_v2 = model == "v2";
        if (config.get(kGeminiConnectionType) == "serial") {
            const std::string port_path = config.get(kGeminiPortPath);
            if (!port_path.empty()) {
                const int baud = static_cast<int>(config.get(kGeminiBaudRate));
                if (is_pro) return vendor::gemini::create_gemini_flatpanel_pro(device_number, port_path, baud);
                if (is_v2) return vendor::gemini::create_gemini_flatpanel_v2(device_number, port_path, baud);
                return vendor::gemini::create_gemini_flatpanel(device_number, port_path, baud);
            }
        }
        const int index = static_cast<int>(config.get(kGeminiPanelIndex));
        if (is_pro) return vendor::gemini::create_gemini_flatpanel_pro_by_index(device_number, index);
        if (is_v2) return vendor::gemini::create_gemini_flatpanel_v2_by_index(device_number, index);
        return vendor::gemini::create_gemini_flatpanel_by_index(device_number, index);
    };
    catalog.add(std::move(panel));

    Factory hub;
    hub.key = DeviceKey{"gemini", DeviceType::Switch};
    hub.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        if (const auto refusal = gemini_switch_refusal(config)) {
            throw AlpacaException(*refusal, AlpacaError::InvalidValue);
        }
        if (config.get(kGeminiConnectionType) == "serial") {
            return vendor::gemini::create_gemini_pdh_switch(device_number, config.get(kGeminiPortPath),
                                                            static_cast<int>(config.get(kGeminiSwitchBaudRate)));
        }
        return vendor::gemini::create_gemini_pdh_switch_by_index(device_number,
                                                                 static_cast<int>(config.get(kGeminiHubIndex)));
    };
    catalog.add(std::move(hub));
}

}  // namespace alpacacore::catalog

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

// The WandererAstro factories (cover calibrator, rotator, filter wheel,
// WandererBox switch), doing what the router arms they replace did. Every
// auto-detect path goes through a create_*_by_index factory, which is
// hardware-free (the scan runs at connect, #659) -- never a resolve_* scan
// function, which would move the scan here, onto the registration path. Compiled
// only under ALPACACORE_ENABLE_WANDERERASTRO (unlike wandererastro_schema.cpp),
// so the vendor headers are fine here.

#include <alpacacore/vendor/wandererastro/wandererastro_box_switch_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_covercalibrator_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_filterwheel_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_rotator_driver.h>

#include "../../catalog/builtin_descriptors.h"
#include "wandererastro_fields.h"

namespace alpacacore::catalog {

namespace {

void refuse_if_invalid(const DeviceConfig& config, const Field<std::int64_t>& index_field, bool is_switch = false) {
    if (const auto refusal = wandererastro_refusal(config, index_field, is_switch)) {
        throw AlpacaException(*refusal, AlpacaError::InvalidValue);
    }
}

bool is_serial(const DeviceConfig& config) { return config.get(kWandererConnectionType) == "serial"; }

int baud_of(const DeviceConfig& config) { return static_cast<int>(config.get(kWandererBaudRate)); }

}  // namespace

void register_wandererastro_factory(DeviceCatalog& catalog) {
    Factory cover;
    cover.key = DeviceKey{"wandererastro", DeviceType::CoverCalibrator};
    cover.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        refuse_if_invalid(config, kWandererCoverIndex);
        if (is_serial(config)) {
            return vendor::wandererastro::create_wandererastro_covercalibrator(
                device_number, config.get(kWandererPortPath), baud_of(config));
        }
        return vendor::wandererastro::create_wandererastro_covercalibrator_by_index(
            device_number, static_cast<int>(config.get(kWandererCoverIndex)));
    };
    catalog.add(std::move(cover));

    Factory rotator;
    rotator.key = DeviceKey{"wandererastro", DeviceType::Rotator};
    rotator.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        refuse_if_invalid(config, kWandererRotatorIndex);
        if (is_serial(config)) {
            return vendor::wandererastro::create_wandererastro_rotator(device_number, config.get(kWandererPortPath),
                                                                       baud_of(config));
        }
        return vendor::wandererastro::create_wandererastro_rotator_by_index(
            device_number, static_cast<int>(config.get(kWandererRotatorIndex)));
    };
    catalog.add(std::move(rotator));

    Factory wheel;
    wheel.key = DeviceKey{"wandererastro", DeviceType::FilterWheel};
    wheel.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        refuse_if_invalid(config, kWandererFilterwheelIndex);
        std::unique_ptr<FilterWheelDriver> driver =
            is_serial(config) ? vendor::wandererastro::create_wandererastro_filterwheel(
                                    device_number, config.get(kWandererPortPath), baud_of(config))
                              : vendor::wandererastro::create_wandererastro_filterwheel_by_index(
                                    device_number, static_cast<int>(config.get(kWandererFilterwheelIndex)));
        if (auto names = config.find(kWandererFilterNames)) driver->set_names(*names);
        return driver;
    };
    catalog.add(std::move(wheel));

    Factory box;
    box.key = DeviceKey{"wandererastro", DeviceType::Switch};
    box.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        refuse_if_invalid(config, kWandererBoxIndex, true);
        if (is_serial(config)) {
            return vendor::wandererastro::create_wandererastro_box_switch(device_number, config.get(kWandererPortPath),
                                                                          baud_of(config));
        }
        return vendor::wandererastro::create_wandererastro_box_switch_by_index(
            device_number, static_cast<int>(config.get(kWandererBoxIndex)));
    };
    catalog.add(std::move(box));
}

}  // namespace alpacacore::catalog

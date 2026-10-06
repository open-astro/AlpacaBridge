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

// The Player One schemas (camera, Phoenix filter wheel, thermal switch). No
// vendor header: compiles in every build (including vendors-OFF), so
// `available` in GET /management/v1/devicecatalog reflects only whether a
// factory is registered.

#include "../../catalog/builtin_descriptors.h"
#include "playerone_fields.h"

namespace alpacacore::catalog {

namespace {

// Two words, so the router's first-word default would say "Player".
constexpr std::string_view kPlayerOneLabel = "Player One";
constexpr std::string_view kPlayerOneBuildOption = "ALPACACORE_ENABLE_PLAYERONE";

void add_schema(DeviceCatalog& catalog, DeviceType type, std::string_view display_name,
                const std::vector<FieldRef>& fields) {
    Schema schema;
    schema.key = DeviceKey{"playerone", type};
    schema.display_name = display_name;
    schema.build_option = kPlayerOneBuildOption;
    schema.fields = fields;
    schema.vendor_label = kPlayerOneLabel;
    catalog.add(std::move(schema));
}

}  // namespace

void register_playerone_schema(DeviceCatalog& catalog) {
    add_schema(catalog, DeviceType::Camera, "Player One Camera", playerone_camera_fields());
    add_schema(catalog, DeviceType::FilterWheel, "Player One Phoenix Filter Wheel", playerone_filterwheel_fields());
    add_schema(catalog, DeviceType::Switch, "Player One Thermal Switch (dew heater/fan)", playerone_switch_fields());
}

}  // namespace alpacacore::catalog

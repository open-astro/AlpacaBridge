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

// The ToupTek schemas (camera, AAF focuser, AFW filter wheel, switch). No vendor
// header: compiles in every build (including vendors-OFF), so `available` in
// GET /management/v1/devicecatalog reflects only whether a factory is
// registered. One switch descriptor serves both backends and branches on
// switchType, as the router arm did; its refusals live in touptek_fields.h so
// the factory throws the same text.

#include <functional>
#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "touptek_fields.h"

namespace alpacacore::catalog {

namespace {

constexpr std::string_view kTouptekBuildOption = "ALPACACORE_ENABLE_TOUPTEK";

void add_schema(DeviceCatalog& catalog, DeviceType type, std::string_view display_name,
                const std::vector<FieldRef>& fields,
                std::function<NormalizeResult(const DeviceConfig&, Source)> normalize = nullptr) {
    Schema schema;
    schema.key = DeviceKey{"touptek", type};
    schema.display_name = display_name;
    schema.build_option = kTouptekBuildOption;
    schema.fields = fields;
    schema.normalize = std::move(normalize);
    catalog.add(std::move(schema));
}

}  // namespace

void register_touptek_schema(DeviceCatalog& catalog) {
    add_schema(catalog, DeviceType::Camera, "ToupTek Camera", touptek_camera_fields());
    add_schema(catalog, DeviceType::Focuser, "ToupTek AAF Focuser", touptek_focuser_fields());
    add_schema(catalog, DeviceType::FilterWheel, "ToupTek AFW Filter Wheel", touptek_filterwheel_fields());
    add_schema(catalog, DeviceType::Switch, "ToupTek Switch", touptek_switch_fields(),
               [](const DeviceConfig& in, Source) {
                   NormalizeResult result;
                   result.config = in;
                   result.rejection = touptek_switch_refusal(in);
                   return result;
               });
}

}  // namespace alpacacore::catalog

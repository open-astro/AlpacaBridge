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

// The WandererAstro schemas (cover calibrator, rotator, filter wheel,
// WandererBox switch). No vendor header: compiles in every build (including
// vendors-OFF), so `available` in GET /management/v1/devicecatalog reflects
// only whether a factory is registered. Each type's cross-field refusals live in
// wandererastro_fields.h.

#include <functional>

#include "../../catalog/builtin_descriptors.h"
#include "wandererastro_fields.h"

namespace alpacacore::catalog {

namespace {

constexpr std::string_view kWandererBuildOption = "ALPACACORE_ENABLE_WANDERERASTRO";

void add_schema(DeviceCatalog& catalog, DeviceType type, std::string_view display_name,
                const std::vector<FieldRef>& fields, const Field<std::int64_t>& index_field, bool is_switch = false) {
    Schema schema;
    schema.key = DeviceKey{"wandererastro", type};
    schema.display_name = display_name;
    schema.build_option = kWandererBuildOption;
    schema.fields = fields;
    schema.normalize = [&index_field, is_switch](const DeviceConfig& in, Source) {
        NormalizeResult result;
        result.config = in;
        result.rejection = wandererastro_refusal(in, index_field, is_switch);
        return result;
    };
    catalog.add(std::move(schema));
}

}  // namespace

void register_wandererastro_schema(DeviceCatalog& catalog) {
    add_schema(catalog, DeviceType::CoverCalibrator, "WandererAstro WandererCover", wandererastro_cover_fields(),
               kWandererCoverIndex);
    add_schema(catalog, DeviceType::Rotator, "WandererAstro WandererRotator Mini", wandererastro_rotator_fields(),
               kWandererRotatorIndex);
    add_schema(catalog, DeviceType::FilterWheel, "WandererAstro SFW Filter Wheel", wandererastro_filterwheel_fields(),
               kWandererFilterwheelIndex);
    add_schema(catalog, DeviceType::Switch, "WandererAstro WandererBox Pro V3", wandererastro_switch_fields(),
               kWandererBoxIndex, true);
}

}  // namespace alpacacore::catalog

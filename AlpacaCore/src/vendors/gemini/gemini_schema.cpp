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

// The Gemini schemas (focuser, flat panel, Power & Data Hub switch). No vendor
// header: compiles in every build (including vendors-OFF), so `available` in
// GET /management/v1/devicecatalog reflects only whether a factory is
// registered. Only the switch has a cross-field refusal (gemini_fields.h); the
// focuser and flat panel fall back to auto-detect when "serial" names no port.

#include <functional>

#include "../../catalog/builtin_descriptors.h"
#include "gemini_fields.h"

namespace alpacacore::catalog {

namespace {

constexpr std::string_view kGeminiBuildOption = "ALPACACORE_ENABLE_GEMINI";

NormalizeResult normalize_gemini_switch(const DeviceConfig& in, Source) {
    NormalizeResult result;
    result.config = in;
    result.rejection = gemini_switch_refusal(in);
    return result;
}

void add_schema(DeviceCatalog& catalog, DeviceType type, std::string_view display_name,
                const std::vector<FieldRef>& fields,
                std::function<NormalizeResult(const DeviceConfig&, Source)> normalize = nullptr) {
    Schema schema;
    schema.key = DeviceKey{"gemini", type};
    schema.display_name = display_name;
    schema.build_option = kGeminiBuildOption;
    schema.fields = fields;
    schema.normalize = std::move(normalize);
    catalog.add(std::move(schema));
}

}  // namespace

void register_gemini_schema(DeviceCatalog& catalog) {
    add_schema(catalog, DeviceType::Focuser, "Gemini Focuser", gemini_focuser_fields());
    add_schema(catalog, DeviceType::CoverCalibrator, "Gemini Flat Panel", gemini_flatpanel_fields());
    add_schema(catalog, DeviceType::Switch, "Gemini Power & Data Hub", gemini_switch_fields(), normalize_gemini_switch);
}

}  // namespace alpacacore::catalog

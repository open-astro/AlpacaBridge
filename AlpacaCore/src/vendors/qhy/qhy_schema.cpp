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

// The QHY schemas (camera, filter wheel, Q-Focuser). No vendor header:
// compiles in every build (including vendors-OFF), so `available` in
// GET /management/v1/devicecatalog reflects only whether a factory is
// registered. The cross-field refusals (a camera handle, the filter wheel's
// wheelType / CFW3 endpoint) live in qhy_fields.h so the factory throws the
// same text; the Q-Focuser has none beyond the per-field ranges.

#include <functional>
#include <optional>
#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "qhy_fields.h"

namespace alpacacore::catalog {

namespace {

constexpr std::string_view kQhyBuildOption = "ALPACACORE_ENABLE_QHY";

// A refusal is rejected from the API; for a saved config the catalog turns it
// into a warning and the device still registers, so it stays listed and
// editable in the web UI (the factory then refuses to build it).
std::function<NormalizeResult(const DeviceConfig&, Source)> refuse_with(
    std::optional<std::string> (*refusal)(const DeviceConfig&)) {
    return [refusal](const DeviceConfig& in, Source) {
        NormalizeResult result;
        result.config = in;
        result.rejection = refusal(in);
        return result;
    };
}

void add_schema(DeviceCatalog& catalog, DeviceType type, std::string_view display_name,
                const std::vector<FieldRef>& fields,
                std::function<NormalizeResult(const DeviceConfig&, Source)> normalize = nullptr) {
    Schema schema;
    schema.key = DeviceKey{"qhy", type};
    schema.display_name = display_name;
    schema.build_option = kQhyBuildOption;
    schema.fields = fields;
    schema.normalize = std::move(normalize);
    catalog.add(std::move(schema));
}

}  // namespace

void register_qhy_schema(DeviceCatalog& catalog) {
    add_schema(catalog, DeviceType::Camera, "QHY Camera", qhy_camera_fields(), refuse_with(qhy_camera_refusal));
    add_schema(catalog, DeviceType::FilterWheel, "QHY Filter Wheel", qhy_filterwheel_fields(),
               refuse_with(qhy_filterwheel_refusal));
    add_schema(catalog, DeviceType::Focuser, "QHY Q-Focuser", qhy_focuser_fields());
}

}  // namespace alpacacore::catalog

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

// The OnStep telescope schema. No vendor header: compiles in every build
// (including vendors-OFF). The display name's first word is the router's
// vendor_label(), so it keeps "OnStep support not enabled ..." as the arm
// spelled it.
//
// Cross-field rules, in the arm's order: connection type, then the serial
// port. The API is refused; a saved config is registered so it stays listed and
// editable in the web UI (#380): an unknown connection type is read as
// "serial", never "auto", so it cannot auto-probe, and an empty port is warned
// about and kept.

#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "onstep_fields.h"

namespace alpacacore::catalog {

namespace {

NormalizeResult normalize_onstep(const DeviceConfig& in, Source source) {
    NormalizeResult result;
    result.config = in;
    const bool from_api = source == Source::Api;

    std::string type = in.get(kOnStepConnectionType);
    if (type != "" && type != "auto" && type != "serial") {
        if (from_api) {
            result.rejection = "Invalid connection type. Use 'auto' or 'serial'";
            return result;
        }
        result.warnings.push_back("saved config has connectionType \"" + type +
                                  "\", which is not one this driver knows; treating it as \"serial\" so the connect "
                                  "fails on the port path instead of auto-probing");
        type = "serial";
        result.config.set(kOnStepConnectionType.key, type);
    }

    if (type == "serial" && in.get(kOnStepPortPath).empty()) {
        constexpr const char* kPortMissing = "Serial port path is required";
        if (from_api) {
            result.rejection = kPortMissing;
            return result;
        }
        result.warnings.emplace_back(kPortMissing);
    }
    return result;
}

}  // namespace

void register_onstep_schema(DeviceCatalog& catalog) {
    Schema schema;
    schema.key = DeviceKey{"onstep", DeviceType::Telescope};
    schema.display_name = "OnStep";
    schema.build_option = "ALPACACORE_ENABLE_ONSTEP";
    schema.fields = onstep_telescope_fields();
    schema.normalize = normalize_onstep;
    catalog.add(std::move(schema));
}

}  // namespace alpacacore::catalog

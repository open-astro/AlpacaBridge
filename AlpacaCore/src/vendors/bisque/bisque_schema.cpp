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

// The Bisque / Paramount (TheSkyX) telescope schema. No vendor header:
// compiles in every build (including vendors-OFF). The display name's first
// word is the router's vendor_label(), so it keeps "Bisque support not
// enabled ..." as the arm spelled it. The registration log line now reads
// "Registered Bisque telescope"; the old arm logged "Registered
// Bisque/Paramount telescope".
//
// One cross-field rule: an empty host. The API is refused; a saved config is
// registered with a warning so it stays listed and editable in the web UI.

#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "bisque_fields.h"

namespace alpacacore::catalog {

namespace {

NormalizeResult normalize_bisque(const DeviceConfig& in, Source source) {
    NormalizeResult result;
    result.config = in;
    std::string endpoint_fault = in.get(kBisqueHost).empty()
                                     ? std::string("Host is required for Bisque/TheSkyX connection")
                                     : network_port_fault(kBisqueTcpPort.key, result.config.get(kBisqueTcpPort));
    if (!endpoint_fault.empty()) {
        if (source == Source::Api) {
            result.rejection = std::move(endpoint_fault);
        } else {
            result.warnings.push_back(std::move(endpoint_fault));
        }
    }
    return result;
}

}  // namespace

void register_bisque_schema(DeviceCatalog& catalog) {
    Schema schema;
    schema.key = DeviceKey{"bisque", DeviceType::Telescope};
    schema.display_name = "Bisque Paramount (TheSkyX)";
    schema.build_option = "ALPACACORE_ENABLE_BISQUE";
    schema.fields = bisque_telescope_fields();
    schema.normalize = normalize_bisque;
    catalog.add(std::move(schema));
}

}  // namespace alpacacore::catalog

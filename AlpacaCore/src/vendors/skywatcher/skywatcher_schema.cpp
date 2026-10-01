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

// The Sky-Watcher direct (motor controller) telescope schema (open-astro#744).
// No vendor header: compiles in every build (including vendors-OFF). The
// display name's first word is the router's vendor_label(), so it keeps
// "SkyWatcher support not enabled ..." and "Registered SkyWatcher telescope"
// as the arm spelled them.
//
// The cross-field rules run in the arm's order: site, connection type,
// endpoint. The API is refused; a saved config is registered so it stays
// listed and editable in the web UI (#380), with these differences per rule:
// a missing site gets no warning here, because the factory logs the #274
// WARNING when it builds the driver; an unknown connection type is read as
// "serial", never "auto", so it cannot auto-probe; an empty endpoint is warned
// about and kept.

#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "skywatcher_fields.h"

namespace alpacacore::catalog {

namespace {

NormalizeResult normalize_skywatcher(const DeviceConfig& in, Source source) {
    NormalizeResult result;
    result.config = in;
    const bool from_api = source == Source::Api;

    if (from_api && (!in.find(kSkyWatcherSiteLatitude) || !in.find(kSkyWatcherSiteLongitude))) {
        result.rejection = std::string(kSkyWatcherMissingSite);
        return result;
    }

    std::string type = in.get(kSkyWatcherConnectionType);
    if (type != "" && type != "auto" && type != "serial" && type != "network") {
        if (from_api) {
            result.rejection = "Invalid connection type. Use 'auto', 'serial', or 'network'";
            return result;
        }
        result.warnings.push_back("saved config has connectionType \"" + type +
                                  "\", which is not one this driver knows; treating it as \"serial\" so the connect "
                                  "fails on the port path instead of auto-probing");
        type = "serial";
        result.config.set(kSkyWatcherConnectionType.key, type);
    }

    const char* endpoint_missing = nullptr;
    if (type == "serial" && in.get(kSkyWatcherPortPath).empty()) {
        endpoint_missing = "Serial port path is required";
    } else if (type == "network" && in.get(kSkyWatcherHost).empty()) {
        endpoint_missing = "Host IP address is required";
    }
    if (endpoint_missing) {
        if (from_api) {
            result.rejection = endpoint_missing;
            return result;
        }
        result.warnings.emplace_back(endpoint_missing);
    }
    return result;
}

}  // namespace

void register_skywatcher_schema(DeviceCatalog& catalog) {
    Schema schema;
    schema.key = DeviceKey{"skywatcher", DeviceType::Telescope};
    schema.display_name = "SkyWatcher direct (motor controller)";
    schema.build_option = "ALPACACORE_ENABLE_SKYWATCHER";
    schema.fields = skywatcher_telescope_fields();
    schema.normalize = normalize_skywatcher;
    catalog.add(std::move(schema));
}

}  // namespace alpacacore::catalog

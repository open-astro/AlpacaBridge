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

// The ZWO schemas (telescope, EFW filter wheel, EAF focuser, CAA rotator,
// switch). No vendor header: compiles in every build (including vendors-OFF), so
// `available` in GET /management/v1/devicecatalog reflects only whether a
// factory is registered. One switch descriptor serves all four backends and
// branches on switchType, as the router arm did. The camera is not here yet.
//
// Telescope rules, in the arm's order: connection type, then the endpoint. The
// API is refused; a saved config is registered so it stays listed and editable
// in the web UI (#380): an unknown or missing connection type is read as
// "serial", never "auto", so it cannot auto-probe, and an empty endpoint is
// warned about and kept.

#include <functional>
#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "zwo_fields.h"

namespace alpacacore::catalog {

namespace {

constexpr std::string_view kZwoBuildOption = "ALPACACORE_ENABLE_ZWO";

void add_schema(DeviceCatalog& catalog, DeviceType type, std::string_view display_name,
                const std::vector<FieldRef>& fields,
                std::function<NormalizeResult(const DeviceConfig&, Source)> normalize = nullptr) {
    Schema schema;
    schema.key = DeviceKey{"zwo", type};
    schema.display_name = display_name;
    schema.build_option = kZwoBuildOption;
    schema.fields = fields;
    schema.normalize = std::move(normalize);
    catalog.add(std::move(schema));
}

NormalizeResult refusal_only(const DeviceConfig& in, std::optional<std::string> refusal) {
    NormalizeResult result;
    result.config = in;
    result.rejection = std::move(refusal);
    return result;
}

NormalizeResult normalize_telescope(const DeviceConfig& in, Source source) {
    NormalizeResult result;
    result.config = in;
    const bool from_api = source == Source::Api;

    // find(), not get(): an absent connectionType is not "auto" for this mount.
    std::string type = in.find(kZwoConnectionType).value_or("");
    if (type != "auto" && type != "serial" && type != "network") {
        if (from_api) {
            result.rejection = "Invalid connection type. Use 'serial', 'network', or 'auto'";
            return result;
        }
        result.warnings.push_back("saved config has connectionType \"" + type +
                                  "\", which is not one this driver knows; treating it as \"serial\" so the connect "
                                  "fails on the port path instead of auto-probing");
        type = "serial";
        result.config.set(kZwoConnectionType.key, type);
    }

    const char* endpoint_missing = nullptr;
    if (type == "serial" && in.get(kZwoPortPath).empty()) {
        endpoint_missing = "Serial port path is required";
    } else if (type == "network" && in.get(kZwoHost).empty()) {
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

void register_zwo_schema(DeviceCatalog& catalog) {
    add_schema(catalog, DeviceType::Telescope, "ZWO AM Mount", zwo_telescope_fields(), normalize_telescope);
    add_schema(catalog, DeviceType::FilterWheel, "ZWO EFW Filter Wheel", zwo_filterwheel_fields(),
               [](const DeviceConfig& in, Source) {
                   return refusal_only(in, zwo_bound_refusal(in, kZwoFilterwheelId, kZwoFilterwheelIndex,
                                                             "ZWO filter wheel requires filterwheelIndex or "
                                                             "filterwheelId"));
               });
    add_schema(catalog, DeviceType::Focuser, "ZWO EAF Focuser", zwo_focuser_fields(),
               [](const DeviceConfig& in, Source) {
                   return refusal_only(in, zwo_bound_refusal(in, kZwoFocuserId, kZwoFocuserIndex,
                                                             "ZWO EAF focuser requires focuserIndex or focuserId"));
               });
    add_schema(catalog, DeviceType::Rotator, "ZWO CAA Rotator", zwo_rotator_fields(),
               [](const DeviceConfig& in, Source) {
                   return refusal_only(in, zwo_bound_refusal(in, kZwoRotatorId, kZwoRotatorIndex,
                                                             "ZWO rotator requires rotatorIndex or rotatorId"));
               });
    add_schema(catalog, DeviceType::Switch, "ZWO Switch", zwo_switch_fields(),
               [](const DeviceConfig& in, Source) { return refusal_only(in, zwo_switch_refusal(in)); });
}

}  // namespace alpacacore::catalog

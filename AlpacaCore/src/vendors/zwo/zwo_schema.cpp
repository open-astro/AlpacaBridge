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

// The ZWO schemas (telescope, camera, EFW filter wheel, EAF focuser, CAA rotator,
// switch). No vendor header: compiles in every build (including vendors-OFF), so
// `available` in GET /management/v1/devicecatalog reflects only whether a
// factory is registered. One switch descriptor serves all four backends and
// branches on switchType, as the router arm did.
//
// Telescope rules, in the arm's order: connection type, then the endpoint. The
// API is refused; a saved config is registered so it stays listed and editable
// in the web UI (#380): a non-empty unknown connection type is read as
// "serial", never "auto", so it cannot auto-probe, and an empty endpoint is
// warned about and kept. An empty or missing connection type is "auto", as for
// every other mount (#508 item 3).

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
    // The ZWO selectors are catalog fields now: a stored one normalize drops is forgotten in the file.
    schema.forget_dropped_selectors = true;
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

    // find(), not get(): an empty or absent connectionType is "auto" (#508 item 3), written
    // into the config so create_telescope sees "auto" rather than reading "" as serial.
    std::string type = in.find(kZwoConnectionType).value_or("");
    if (type.empty()) {
        type = "auto";
        result.config.set(kZwoConnectionType.key, type);
    }
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

// The camera's selector range, identity strings and selector rule, in the router arm's
// order. The API refuses an invalid value ("Invalid value for <field>"); a saved config
// loses it and relearns it at registration, so the camera stays listed.
NormalizeResult normalize_camera(const DeviceConfig& in, Source source) {
    NormalizeResult result;
    result.config = in;
    // The selectors first, cameraId before cameraIndex, as the arm read them.
    for (const Field<std::int64_t>* selector : {&kZwoCamId, &kZwoCamIndex}) {
        const std::optional<std::int64_t> value = in.find(*selector);
        if (!value || (*value >= kZwoSelectorMin && *value <= kZwoSelectorMax)) continue;
        if (source == Source::Api) {
            result.rejection = std::string("Invalid value for ") + selector->key + ": expected an integer from -1 to " +
                               std::to_string(kZwoSelectorMax);
            return result;
        }
        result.warnings.push_back(std::string(selector->key) + " is outside -1 to " + std::to_string(kZwoSelectorMax));
        result.config.erase(selector->key);
    }
    const struct {
        const Field<std::string>& field;
        bool (*valid)(const std::string&);
        const char* why;
    } checks[] = {{kZwoCamSerial, zwo_serial_valid, "is not a hexadecimal serial of at most 64 digits"},
                  {kZwoCamName, zwo_identity_text_valid, "contains control characters"}};
    for (const auto& check : checks) {
        if (check.valid(in.get(check.field))) continue;
        if (source == Source::Api) {
            result.rejection = std::string("Invalid value for ") + check.field.key;
            return result;
        }
        result.warnings.push_back(std::string(check.field.key) + " " + check.why);
        result.config.erase(check.field.key);
    }
    if (source == Source::Api && result.config.get(kZwoCamId) < 0 && result.config.get(kZwoCamIndex) < 0 &&
        result.config.get(kZwoCamSerial).empty() && result.config.get(kZwoCamName).empty()) {
        result.rejection = "ZWO camera requires cameraIndex or cameraId";
    }
    return result;
}

}  // namespace

void register_zwo_schema(DeviceCatalog& catalog) {
    add_schema(catalog, DeviceType::Telescope, "ZWO AM Mount", zwo_telescope_fields(), normalize_telescope);
    add_schema(catalog, DeviceType::Camera, "ZWO Camera", zwo_camera_fields(), normalize_camera);
    add_schema(catalog, DeviceType::FilterWheel, "ZWO EFW Filter Wheel", zwo_filterwheel_fields(),
               [](const DeviceConfig& in, Source source) {
                   return refusal_only(
                       in, source == Source::Api
                               ? zwo_bound_refusal(in, kZwoFilterwheelId, kZwoFilterwheelIndex,
                                                   "ZWO filter wheel requires filterwheelIndex or filterwheelId")
                               : std::nullopt);
               });
    add_schema(catalog, DeviceType::Focuser, "ZWO EAF Focuser", zwo_focuser_fields(),
               [](const DeviceConfig& in, Source source) {
                   return refusal_only(in, source == Source::Api
                                               ? zwo_bound_refusal(in, kZwoFocuserId, kZwoFocuserIndex,
                                                                   "ZWO EAF focuser requires focuserIndex or focuserId")
                                               : std::nullopt);
               });
    add_schema(catalog, DeviceType::Rotator, "ZWO CAA Rotator", zwo_rotator_fields(),
               [](const DeviceConfig& in, Source source) {
                   return refusal_only(in, source == Source::Api
                                               ? zwo_bound_refusal(in, kZwoRotatorId, kZwoRotatorIndex,
                                                                   "ZWO rotator requires rotatorIndex or rotatorId")
                                               : std::nullopt);
               });
    add_schema(catalog, DeviceType::Switch, "ZWO Switch", zwo_switch_fields(),
               [](const DeviceConfig& in, Source source) {
                   if (auto refusal = zwo_switch_refusal(in)) return refusal_only(in, std::move(refusal));
                   // A dew heater with no camera selector is Auto (cameraIndex -1): the one
                   // attached camera with a heater. A bad serial is refused from the API and
                   // dropped from a saved entry, which then binds by index.
                   NormalizeResult result;
                   result.config = in;
                   if (zwo_switch_type(in) == "dewheater" && !zwo_serial_valid(in.get(kZwoSwitchSerial))) {
                       if (source == Source::Api) {
                           result.rejection = "Invalid value for serialNumber";
                           return result;
                       }
                       result.warnings.emplace_back("Ignoring invalid ZWO dew heater serialNumber in the stored entry");
                       result.config.erase(kZwoSwitchSerial.key);
                   }
                   return result;
               });
}

}  // namespace alpacacore::catalog

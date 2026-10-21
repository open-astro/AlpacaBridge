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

// The iOptron schemas (telescope, iMate PowerBox switch, iEAF focuser, iEFW
// filter wheel, iGuider guide camera, iCAM camera). No vendor header: compiles in every build
// (including vendors-OFF), so `available` in GET /management/v1/devicecatalog
// reflects only whether a factory is registered. The iCAM camera is a rebadged
// Player One, so it is built by ALPACACORE_ENABLE_PLAYERONE, not the iOptron option.

#include <functional>

#include "../../catalog/builtin_descriptors.h"
#include "ioptron_fields.h"

namespace alpacacore::catalog {

namespace {

constexpr std::string_view kIoptronBuildOption = "ALPACACORE_ENABLE_IOPTRON";
constexpr std::string_view kIoptronCameraBuildOption = "ALPACACORE_ENABLE_PLAYERONE";

NormalizeResult normalize_ioptron_telescope(const DeviceConfig& in, Source source) {
    NormalizeResult result;
    result.config = in;
    const bool from_api = source == Source::Api;

    // An out-of-range site coordinate is refused from the API and dropped from a
    // saved config (the field falls back to unset, as for the arm it replaces).
    const std::pair<const Field<double>*, double> sites[] = {{&kIoptronSiteLatitude, 90.0},
                                                             {&kIoptronSiteLongitude, 180.0}};
    for (const auto& [field, limit] : sites) {
        if (const auto refusal = ioptron_site_refusal(*field, in, limit)) {
            if (from_api) {
                result.rejection = *refusal;
                return result;
            }
            result.warnings.push_back(*refusal);
            result.config.erase(field->key);
        }
    }

    std::string type = in.get(kIoptronConnectionType);
    if (type != "" && type != "auto" && type != "serial" && type != "network") {
        if (from_api) {
            result.rejection = "Invalid connection type. Use 'auto', 'serial', or 'network'";
            return result;
        }
        result.warnings.push_back("saved config has connectionType \"" + type +
                                  "\", which is not one this driver knows; treating it as \"serial\" so the connect "
                                  "fails on the port path instead of auto-probing");
        type = "serial";
        result.config.set(kIoptronConnectionType.key, type);
    }
    if (type == "serial" && in.get(kIoptronPortPath).empty()) {
        if (from_api) {
            result.rejection = "Serial port path is required";
            return result;
        }
        result.warnings.emplace_back("Serial port path is required");
    }
    return result;
}

NormalizeResult normalize_ioptron_filterwheel(const DeviceConfig& in, Source) {
    NormalizeResult result;
    result.config = in;
    result.rejection = ioptron_filterwheel_refusal(in);
    return result;
}

NormalizeResult normalize_iguider_camera(const DeviceConfig& in, Source) {
    NormalizeResult result;
    result.config = in;
    result.rejection = iguider_camera_refusal(in);
    return result;
}

NormalizeResult normalize_ioptron_switch(const DeviceConfig& in, Source) {
    NormalizeResult result;
    result.config = in;
    result.rejection = ioptron_switch_refusal(in);
    return result;
}

void add_schema(DeviceCatalog& catalog, DeviceType type, std::string_view display_name, std::string_view build_option,
                const std::vector<FieldRef>& fields,
                std::function<NormalizeResult(const DeviceConfig&, Source)> normalize = nullptr) {
    Schema schema;
    schema.key = DeviceKey{"ioptron", type};
    schema.display_name = display_name;
    schema.build_option = build_option;
    schema.fields = fields;
    schema.normalize = std::move(normalize);
    catalog.add(std::move(schema));
}

}  // namespace

void register_ioptron_schema(DeviceCatalog& catalog) {
    add_schema(catalog, DeviceType::Telescope, "iOptron Mount", kIoptronBuildOption, ioptron_telescope_fields(),
               normalize_ioptron_telescope);
    add_schema(catalog, DeviceType::Switch, "iOptron iMate PowerBox", kIoptronBuildOption, ioptron_switch_fields(),
               normalize_ioptron_switch);
    add_schema(catalog, DeviceType::Focuser, "iOptron iEAF Focuser", kIoptronBuildOption, ioptron_focuser_fields());
    add_schema(catalog, DeviceType::FilterWheel, "iOptron iEFW Filter Wheel", kIoptronBuildOption,
               ioptron_filterwheel_fields(), normalize_ioptron_filterwheel);
    add_schema(catalog, DeviceType::Camera, "iOptron iCAM Camera", kIoptronCameraBuildOption, ioptron_camera_fields());

    // The iGuider guide camera is its own vendor entry ("iOptron iGuider" in the
    // web UI) so it is never confused with the iCAM imaging cameras above. It is
    // a UVC camera built with the iOptron option, not the Player One one.
    Schema iguider;
    iguider.key = DeviceKey{"iguider", DeviceType::Camera};
    iguider.display_name = "iOptron iGuider Camera";
    iguider.build_option = kIoptronBuildOption;
    iguider.vendor_label = "iOptron iGuider";
    iguider.fields = iguider_camera_fields();
    iguider.normalize = normalize_iguider_camera;
    catalog.add(std::move(iguider));
}

}  // namespace alpacacore::catalog

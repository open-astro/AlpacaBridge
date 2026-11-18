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

// The OnStep telescope, focuser, rotator and switch schemas (the other three share the telescope's connection rules,
// #2223, #2221, #2222). No vendor
// header: compiles in every build
// (including vendors-OFF). The display name's first word is the router's
// vendor_label(), so it keeps "OnStep support not enabled ..." as the arm
// spelled it.
//
// Cross-field rules, in the arm's order: connection type, then the endpoint
// (the serial port, or the network host and TCP port, #2220). The API is
// refused; a saved config is registered so it stays listed and editable in the
// web UI (#380): an unknown connection type is read as "serial", never "auto",
// so it cannot auto-probe, and an empty port or host, or a TCP port outside
// 1-65535, is warned about and kept, so the connect refuses it. "auto" scans the
// serial ports only; nothing scans the network.

#include <cstdint>
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
    if (type != "" && type != "auto" && type != "serial" && type != "network") {
        if (from_api) {
            result.rejection = "Invalid connection type. Use 'auto', 'serial', or 'network'";
            return result;
        }
        result.warnings.push_back("saved config has connectionType \"" + type +
                                  "\", which is not one this driver knows; treating it as \"serial\" so the connect "
                                  "fails on the port path instead of auto-probing");
        type = "serial";
        result.config.set(kOnStepConnectionType.key, type);
    }

    std::string endpoint_fault;
    if (type == "serial" && in.get(kOnStepPortPath).empty()) {
        endpoint_fault = "Serial port path is required";
    } else if (type == "network" && in.get(kOnStepHost).empty()) {
        endpoint_fault = "Host IP address is required";
    } else if (type == "network") {
        // The per-field rule has already refused (API) or dropped (saved) a port of the wrong type.
        const std::int64_t port = result.config.get(kOnStepTcpPort);
        if (port < kOnStepTcpPortMin || port > kOnStepTcpPortMax) {
            endpoint_fault = std::string(kOnStepTcpPort.key) + " is out of range (min " +
                             std::to_string(kOnStepTcpPortMin) + ") (max " + std::to_string(kOnStepTcpPortMax) + ")";
        }
    }
    if (!endpoint_fault.empty()) {
        if (from_api) {
            result.rejection = endpoint_fault;
            return result;
        }
        result.warnings.push_back(std::move(endpoint_fault));
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

    Schema focuser;
    focuser.key = DeviceKey{"onstep", DeviceType::Focuser};
    focuser.display_name = "OnStep";
    focuser.build_option = "ALPACACORE_ENABLE_ONSTEP";
    focuser.fields = onstep_focuser_fields();
    focuser.normalize = normalize_onstep;
    catalog.add(std::move(focuser));

    Schema rotator;
    rotator.key = DeviceKey{"onstep", DeviceType::Rotator};
    rotator.display_name = "OnStep";
    rotator.build_option = "ALPACACORE_ENABLE_ONSTEP";
    rotator.fields = onstep_rotator_fields();
    rotator.normalize = normalize_onstep;
    catalog.add(std::move(rotator));

    Schema aux_switch;
    aux_switch.key = DeviceKey{"onstep", DeviceType::Switch};
    aux_switch.display_name = "OnStep";
    aux_switch.build_option = "ALPACACORE_ENABLE_ONSTEP";
    aux_switch.fields = onstep_switch_fields();
    aux_switch.normalize = normalize_onstep;
    catalog.add(std::move(aux_switch));
}

}  // namespace alpacacore::catalog

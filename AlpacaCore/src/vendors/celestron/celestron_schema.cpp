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

// The Celestron telescope and focus motor schemas. No vendor header: compiles in every build
// (including vendors-OFF). The display name's first word is the router's
// vendor_label(), so it keeps "Celestron support not enabled ..." as the arm
// spelled it.
//
// Cross-field rules: protocol selection (NexStar default or CGX AUX), alignment
// mode (#860), connection type (the CGX: USB or SkyPortal WiFi), endpoint (the
// CGX WiFi host: a numeric address), and the direct backend's required site
// coordinates. The API is refused; a saved config is registered so
// it stays listed and editable in the web UI (#380): an unknown connection type
// is read as "serial", never "auto", so it cannot auto-probe, and an empty
// endpoint is warned about and kept. An unknown alignmentMode is dropped from
// either source and reads as "auto" (#860).

#include <alpacacore/util/alignment_mode.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "celestron_fields.h"

namespace alpacacore::catalog {

namespace {

// A numeric IPv4 or IPv6 address, as the CGX TCP connect parses it. inet_pton stops at a NUL, so one is refused.
bool is_numeric_ip(const std::string& host) {
    if (host.find('\0') != std::string::npos) return false;
    in6_addr address{};
    return ::inet_pton(AF_INET, host.c_str(), &address) == 1 || ::inet_pton(AF_INET6, host.c_str(), &address) == 1;
}

NormalizeResult normalize_celestron(const DeviceConfig& in, Source source) {
    NormalizeResult result;
    result.config = in;
    const bool from_api = source == Source::Api;

    // The router already drops a wrong-typed or unknown alignmentMode before the
    // typed read (without_unknown_alignment_mode); this check stays so the schema
    // keeps the #860 rule for every caller that does not go through that helper.
    if (in.find(kCelestronAlignmentMode)) {
        const std::string mode = in.get(kCelestronAlignmentMode);
        if (!util::is_known_alignment_mode(mode)) {
            result.config.erase(kCelestronAlignmentMode.key);
        }
    }

    const std::string protocol = result.config.get(kCelestronProtocol);
    std::string type = in.get(kCelestronConnectionType);
    // The CGX has two transports: "serial" is the direct USB port, "network" the SkyPortal WiFi adapter. A saved
    // entry from before WiFi has "serial", and one with no connection type (or another value) is read as USB.
    if (protocol == "cgx-aux" && type != "serial" && type != "network") {
        if (from_api) {
            result.rejection =
                "Celestron CGX AUX requires Connection Type 'serial' (direct USB) or 'network' (SkyPortal WiFi)";
            return result;
        }
        result.warnings.push_back("saved cgx-aux protocol has no USB or WiFi connection type; using serial (USB)");
        type = "serial";
        result.config.set(kCelestronConnectionType.key, type);
    }
    if (protocol == "cgx-aux" &&
        (!in.find(kCelestronSiteLatitude).has_value() || !in.find(kCelestronSiteLongitude).has_value())) {
        const char* message = type == "network" ? "Celestron CGX over WiFi requires SiteLatitude and SiteLongitude"
                                                : "Direct Celestron CGX USB requires SiteLatitude and SiteLongitude";
        if (from_api) {
            result.rejection = message;
            return result;
        }
        result.warnings.emplace_back(message);
    }
    if (type != "" && type != "auto" && type != "serial" && type != "network") {
        if (from_api) {
            result.rejection = "Invalid connection type. Use 'auto', 'serial', or 'network'";
            return result;
        }
        result.warnings.push_back("saved config has connectionType \"" + type +
                                  "\", which is not one this driver knows; treating it as \"serial\" so the connect "
                                  "fails on the port path instead of auto-probing");
        type = "serial";
        result.config.set(kCelestronConnectionType.key, type);
    }

    std::string endpoint_missing;
    if (type == "serial" && in.get(kCelestronPortPath).empty()) {
        endpoint_missing = "Serial port path is required";
    } else if (type == "network" && in.get(kCelestronHost).empty()) {
        endpoint_missing = "Host IP address is required";
    } else if (type == "network" && protocol == "cgx-aux" && !is_numeric_ip(in.get(kCelestronHost))) {
        // The CGX link names no host it would have to resolve under its I/O lock (celestron_cgx_protocol_wrapper.cpp).
        endpoint_missing = "Celestron CGX over WiFi needs the SkyPortal adapter's numeric IPv4 or IPv6 address "
                           "(1.2.3.4 on the adapter's own network)";
    } else if (type == "network") {
        endpoint_missing = network_port_fault(kCelestronTcpPort.key, result.config.get(kCelestronTcpPort));
    }
    if (!endpoint_missing.empty()) {
        if (from_api) {
            result.rejection = endpoint_missing;
            return result;
        }
        result.warnings.emplace_back(endpoint_missing);
    }
    return result;
}

// The focus motor has no auto-detect, so it needs its port (issue #896). A saved
// entry without one is kept and warned about, as the telescope does (#380).
NormalizeResult normalize_celestron_focuser(const DeviceConfig& in, Source source) {
    NormalizeResult result;
    result.config = in;
    if (in.get(kCelestronFocuserPortPath).empty()) {
        const char* message = "Serial port path is required for the Celestron focus motor";
        if (source == Source::Api) {
            result.rejection = message;
        } else {
            result.warnings.emplace_back(message);
        }
    }
    return result;
}

}  // namespace

void register_celestron_schema(DeviceCatalog& catalog) {
    Schema schema;
    schema.key = DeviceKey{"celestron", DeviceType::Telescope};
    schema.display_name = "Celestron";
    schema.build_option = "ALPACACORE_ENABLE_CELESTRON";
    schema.fields = celestron_telescope_fields();
    schema.normalize = normalize_celestron;
    catalog.add(std::move(schema));

    Schema focuser;
    focuser.key = DeviceKey{"celestron", DeviceType::Focuser};
    focuser.display_name = "Celestron Focus Motor";
    focuser.build_option = "ALPACACORE_ENABLE_CELESTRON";
    focuser.fields = celestron_focuser_fields();
    focuser.normalize = normalize_celestron_focuser;
    catalog.add(std::move(focuser));
}

}  // namespace alpacacore::catalog

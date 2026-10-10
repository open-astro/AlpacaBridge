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

#pragma once

// Gemini catalog field declarations (focuser, flat panel, Power & Data Hub
// switch), shared by gemini_schema.cpp (no vendor header) and gemini_catalog.cpp
// (the factory, vendor header allowed). No vendor header here either: the schema
// file includes this one and compiles in every build (including vendors-OFF).

#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Designated initializers of Field<T> leave most members defaulted on purpose
// (matches AlpacaHTTP/tests/test_catalog_descriptor.h).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

// "auto" (or unset) scans at connect; "serial" names the port. A focuser or flat
// panel set to "serial" with no port path auto-detects, as the router arm did.
inline const Field<std::string> kGeminiConnectionType{
    .key = "connectionType", .default_value = "auto", .role = Role::Discriminator};
inline const Field<std::string> kGeminiPortPath{.key = "portPath",
                                                .default_value = "",
                                                .role = Role::PortPath,
                                                .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::int64_t> kGeminiBaudRate{
    .key = "baudRate", .default_value = 9600, .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::int64_t> kGeminiFocuserIndex{
    .key = "focuserIndex", .default_value = 0, .role = Role::EnumerationIndex};

// "lite" (Cover Lite, the default), "v2" (Automatic FlatPanel v2) or "pro"
// (Motorized Flat Panel V3). Not an allowed_values list: an unknown value has
// always meant "lite", and the factory keeps that.
inline const Field<std::string> kGeminiFlatPanelModel{
    .key = "flatPanelModel", .default_value = "lite", .role = Role::Discriminator};
inline const Field<std::int64_t> kGeminiPanelIndex{
    .key = "panelIndex", .default_value = 0, .role = Role::EnumerationIndex};

// Power & Data Hub switch: its own baud default, and the only backend today is
// "pdh-adv3" (the PowerBox Mini 2 is a candidate second one).
inline const Field<std::string> kGeminiSwitchType{
    .key = "switchType", .default_value = "pdh-adv3", .role = Role::Discriminator};
inline const Field<std::int64_t> kGeminiSwitchBaudRate{
    .key = "baudRate", .default_value = 19200, .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::int64_t> kGeminiHubIndex{.key = "hubIndex", .default_value = 0, .role = Role::EnumerationIndex};

inline const std::vector<FieldRef>& gemini_focuser_fields() {
    static const std::vector<FieldRef> fields{kGeminiConnectionType.ref(), kGeminiPortPath.ref(), kGeminiBaudRate.ref(),
                                              kGeminiFocuserIndex.ref()};
    return fields;
}

inline const std::vector<FieldRef>& gemini_flatpanel_fields() {
    static const std::vector<FieldRef> fields{kGeminiConnectionType.ref(), kGeminiPortPath.ref(), kGeminiBaudRate.ref(),
                                              kGeminiFlatPanelModel.ref(), kGeminiPanelIndex.ref()};
    return fields;
}

inline const std::vector<FieldRef>& gemini_switch_fields() {
    static const std::vector<FieldRef> fields{kGeminiSwitchType.ref(), kGeminiConnectionType.ref(),
                                              kGeminiPortPath.ref(), kGeminiSwitchBaudRate.ref(),
                                              kGeminiHubIndex.ref()};
    return fields;
}

// The switch's cross-field refusals, in the router arm's order. The schema's
// normalize reports them (rejected from the API, warned about for a saved
// config) and the factory throws the same text, so a saved config normalize
// could only warn about is never built into a hub that would auto-detect behind
// the user's back.
inline std::optional<std::string> gemini_switch_refusal(const DeviceConfig& config) {
    const std::string switch_type = config.get(kGeminiSwitchType);
    if (switch_type != "pdh-adv3") {
        return "Unknown Gemini switchType: " + switch_type + " (supported: pdh-adv3)";
    }
    if (config.get(kGeminiConnectionType) == "serial") {
        // Serial mode means an explicit port: no silent auto-detect.
        if (config.get(kGeminiPortPath).empty()) {
            return "portPath is required when connectionType is 'serial' (or use 'auto').";
        }
    } else if (config.get(kGeminiHubIndex) < 0) {
        return "hubIndex must be >= 0.";
    }
    return std::nullopt;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

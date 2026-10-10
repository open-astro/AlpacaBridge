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

// WandererAstro catalog field declarations (cover calibrator, rotator, filter
// wheel, WandererBox switch), shared by wandererastro_schema.cpp (no vendor
// header) and wandererastro_catalog.cpp (the factory, vendor header allowed).
// No vendor header here either: the schema file includes this one and compiles
// in every build (including vendors-OFF).

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

// "auto" (or unset) scans at connect; "serial" names the port, and a serial
// config with no port is refused (wandererastro_refusal) rather than silently
// auto-detecting.
inline const Field<std::string> kWandererConnectionType{
    .key = "connectionType", .default_value = "auto", .role = Role::Discriminator};
inline const Field<std::string> kWandererPortPath{.key = "portPath",
                                                  .default_value = "",
                                                  .role = Role::PortPath,
                                                  .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::int64_t> kWandererBaudRate{
    .key = "baudRate", .default_value = 19200, .applies_when = AppliesWhen{"connectionType", "serial"}};

inline const Field<std::int64_t> kWandererCoverIndex{
    .key = "coverIndex", .default_value = 0, .role = Role::EnumerationIndex};
inline const Field<std::int64_t> kWandererRotatorIndex{
    .key = "rotatorIndex", .default_value = 0, .role = Role::EnumerationIndex};
inline const Field<std::int64_t> kWandererFilterwheelIndex{
    .key = "wandererFilterwheelIndex", .default_value = 0, .role = Role::EnumerationIndex};
inline const Field<std::int64_t> kWandererBoxIndex{
    .key = "boxIndex", .default_value = 0, .role = Role::EnumerationIndex};

// Absent filterNames leaves the driver's defaults; a non-array or non-string
// element is rejected by the declared StringList kind.
inline const Field<std::vector<std::string>> kWandererFilterNames{.key = "filterNames", .default_value = {}};

// Backend selector of the switch; only the WandererBox Pro V3 exists today (the
// ETA tilt adjuster is planned as a second backend).
inline const Field<std::string> kWandererSwitchType{
    .key = "switchType", .default_value = "wandererbox-pro-v3", .role = Role::Discriminator};

inline const std::vector<FieldRef>& wandererastro_cover_fields() {
    static const std::vector<FieldRef> fields{kWandererConnectionType.ref(), kWandererPortPath.ref(),
                                              kWandererBaudRate.ref(), kWandererCoverIndex.ref()};
    return fields;
}

inline const std::vector<FieldRef>& wandererastro_rotator_fields() {
    static const std::vector<FieldRef> fields{kWandererConnectionType.ref(), kWandererPortPath.ref(),
                                              kWandererBaudRate.ref(), kWandererRotatorIndex.ref()};
    return fields;
}

inline const std::vector<FieldRef>& wandererastro_filterwheel_fields() {
    static const std::vector<FieldRef> fields{kWandererConnectionType.ref(), kWandererPortPath.ref(),
                                              kWandererBaudRate.ref(), kWandererFilterwheelIndex.ref(),
                                              kWandererFilterNames.ref()};
    return fields;
}

inline const std::vector<FieldRef>& wandererastro_switch_fields() {
    static const std::vector<FieldRef> fields{kWandererSwitchType.ref(), kWandererConnectionType.ref(),
                                              kWandererPortPath.ref(), kWandererBaudRate.ref(),
                                              kWandererBoxIndex.ref()};
    return fields;
}

// The cross-field refusals shared by all four types, in the router arms' order
// (switchType first for the switch, then the port, then the auto-detect index).
// The schema's normalize reports them (rejected from the API, warned about for a
// saved config) and the factory throws the same text, so a saved config
// normalize could only warn about is never built into a device that would
// auto-detect behind the user's back. `switch_type` is null for the other three.
inline std::optional<std::string> wandererastro_refusal(const DeviceConfig& config,
                                                        const Field<std::int64_t>& index_field,
                                                        bool is_switch = false) {
    if (is_switch) {
        const std::string switch_type = config.get(kWandererSwitchType);
        if (switch_type != "wandererbox-pro-v3") {
            return "Unknown WandererAstro switchType: " + switch_type + " (supported: wandererbox-pro-v3)";
        }
    }
    if (config.get(kWandererConnectionType) == "serial") {
        if (config.get(kWandererPortPath).empty()) {
            return "portPath is required when connectionType is 'serial' (or use 'auto').";
        }
    } else if (config.get(index_field) < 0) {
        return std::string(index_field.key) + " must be >= 0.";
    }
    return std::nullopt;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

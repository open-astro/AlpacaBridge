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

// ToupTek catalog field declarations (camera, AAF focuser, AFW filter wheel,
// switch), shared by touptek_schema.cpp (no vendor header) and
// touptek_catalog.cpp (the factories, vendor header allowed). No vendor header
// here either: the schema file includes this one and compiles in every build
// (including vendors-OFF).

#include <alpacacore/catalog/device_catalog.h>
#include <alpacacore/util/hardware_config_refusal.h>

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

// Camera, and the thermal switch's camera handle. SDK enumeration index.
inline const Field<std::int64_t> kTouptekCameraIndex{
    .key = "cameraIndex", .default_value = 0, .role = Role::EnumerationIndex};

// AAF focuser: the SDK id string wins over the index when non-empty.
inline const Field<std::int64_t> kTouptekFocuserIndex{
    .key = "focuserIndex", .default_value = 0, .role = Role::EnumerationIndex};
inline const Field<std::string> kTouptekFocuserId{.key = "focuserId", .default_value = "", .role = Role::DeviceId};

// AFW filter wheel: same shape. Absent filterNames leaves the driver's defaults.
inline const Field<std::int64_t> kTouptekWheelIndex{
    .key = "filterwheelIndex", .default_value = 0, .role = Role::EnumerationIndex};
inline const Field<std::string> kTouptekWheelId{.key = "filterwheelId", .default_value = "", .role = Role::DeviceId};
inline const Field<std::vector<std::string>> kTouptekFilterNames{.key = "filterNames", .default_value = {}};

// Switch. Two backends share (touptek, switch): "thermal" (a cooled camera's dew
// heater + fan through the camera SDK, any ToupTek build) and "stellavita" (the
// default; the StellaVita PowerBox's GPIO ports, only where libgpiod is built).
// normalize owns the enum rule so the refusal keeps its wording.
inline const Field<std::string> kTouptekSwitchType{
    .key = "switchType", .default_value = "stellavita", .role = Role::Discriminator};
inline const Field<std::int64_t> kTouptekThermalCameraIndex{.key = "cameraIndex",
                                                            .default_value = 0,
                                                            .role = Role::EnumerationIndex,
                                                            .applies_when = AppliesWhen{"switchType", "thermal"}};
inline const Field<std::string> kTouptekGpioChip{
    .key = "gpioChip", .default_value = "/dev/gpiochip0", .applies_when = AppliesWhen{"switchType", "stellavita"}};
inline const Field<std::int64_t> kTouptekPwmFrequencyHz{
    .key = "pwmFrequencyHz", .default_value = 100, .applies_when = AppliesWhen{"switchType", "stellavita"}};

// One entry of ports[]: applied positionally onto the fixed Port 1..4 layout.
// An absent name / pwm leaves the port's own default.
inline const Field<std::string> kTouptekPortName{.key = "name", .default_value = ""};
inline const Field<bool> kTouptekPortPwm{.key = "pwm", .default_value = false};
inline const std::vector<FieldRef>& touptek_port_fields() {
    static const std::vector<FieldRef> fields{kTouptekPortName.ref(), kTouptekPortPwm.ref()};
    return fields;
}
inline const Field<std::vector<DeviceConfig>> kTouptekPorts{.key = "ports",
                                                            .default_value = {},
                                                            .applies_when = AppliesWhen{"switchType", "stellavita"},
                                                            .record_fields = touptek_port_fields()};

inline const std::vector<FieldRef>& touptek_camera_fields() {
    static const std::vector<FieldRef> fields{kTouptekCameraIndex.ref()};
    return fields;
}

inline const std::vector<FieldRef>& touptek_focuser_fields() {
    static const std::vector<FieldRef> fields{kTouptekFocuserIndex.ref(), kTouptekFocuserId.ref()};
    return fields;
}

inline const std::vector<FieldRef>& touptek_filterwheel_fields() {
    static const std::vector<FieldRef> fields{kTouptekWheelIndex.ref(), kTouptekWheelId.ref(),
                                              kTouptekFilterNames.ref()};
    return fields;
}

inline const std::vector<FieldRef>& touptek_switch_fields() {
    static const std::vector<FieldRef> fields{kTouptekSwitchType.ref(), kTouptekThermalCameraIndex.ref(),
                                              kTouptekGpioChip.ref(), kTouptekPwmFrequencyHz.ref(),
                                              kTouptekPorts.ref()};
    return fields;
}

// The switch's cross-field refusals, in the router arm's order. normalize reports
// them (rejected from the API, warned about for a saved config) and the factory
// throws the same text, so a saved config normalize could only warn about is
// never built into a driver. The gpioChip check exists only where the StellaVita
// backend is built (ALPACACORE_TOUPTEK_STELLAVITA, defined for the builtins
// library), as the router arm only ran it there.
inline std::optional<std::string> touptek_switch_refusal([[maybe_unused]] const DeviceConfig& config) {
    const std::string type = config.get(kTouptekSwitchType);
    if (type != "thermal" && type != "stellavita") {
        return "Unknown ToupTek switchType '" + type + "' (expected 'thermal' or 'stellavita')";
    }
#ifdef ALPACACORE_TOUPTEK_STELLAVITA
    if (type == "stellavita") {
        const std::string chip = config.get(kTouptekGpioChip);
        if (chip != "/dev/gpiochip0") return util::hardware_config_refusal("gpioChip", "'/dev/gpiochip0'");
    }
#endif
    return std::nullopt;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

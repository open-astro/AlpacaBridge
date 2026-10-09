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

#pragma once

// QHY catalog field declarations (camera, filter wheel, Q-Focuser), shared by
// qhy_schema.cpp (no vendor header) and qhy_catalog.cpp (the factories, vendor
// header allowed). No vendor header here either: the schema file includes this
// one and compiles in every build (including vendors-OFF).

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

// Camera handle: an SDK enumeration index or the camera's string id (QHY ids
// are char[32]). The camera and the integrated filter wheel (which shares the
// camera's handle) are bound the same way. A negative index counts as unset.
inline const Field<std::int64_t> kQhyCameraIndex{
    .key = "cameraIndex", .default_value = 0, .role = Role::EnumerationIndex};
inline const Field<std::string> kQhyCameraId{.key = "cameraId", .default_value = "", .role = Role::DeviceId};

// Filter wheel. "integrated" (the default, and what every config saved before
// the CFW3 USB driver means) or "cfw3-usb"; normalize owns the rule so the
// refusal keeps its wording.
inline const Field<std::string> kQhyWheelType{
    .key = "wheelType", .default_value = "integrated", .role = Role::Discriminator};
inline const Field<std::string> kQhyWheelConnectionType{.key = "connectionType",
                                                        .default_value = "auto",
                                                        .role = Role::Discriminator,
                                                        .applies_when = AppliesWhen{"wheelType", "cfw3-usb"}};
inline const Field<std::string> kQhyWheelPortPath{.key = "portPath",
                                                  .default_value = "",
                                                  .role = Role::PortPath,
                                                  .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::int64_t> kQhyWheelIndex{.key = "filterwheelIndex",
                                                .default_value = 0,
                                                .role = Role::EnumerationIndex,
                                                .applies_when = AppliesWhen{"wheelType", "cfw3-usb"}};
// Slot names. Absent leaves the driver's own defaults ("Filter 1..N").
inline const Field<std::vector<std::string>> kQhyFilterNames{.key = "filterNames", .default_value = {}};

// Q-Focuser: USB CDC-ACM serial at a fixed 9600 baud, no SDK. The defaults are
// QFocuserSettings' own; the motion/hold settings are pushed at every connect.
inline const Field<std::string> kQhyFocuserConnectionType{
    .key = "connectionType", .default_value = "auto", .role = Role::Discriminator};
inline const Field<std::string> kQhyFocuserPortPath{.key = "portPath",
                                                    .default_value = "",
                                                    .role = Role::PortPath,
                                                    .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::int64_t> kQhyFocuserIndex{
    .key = "focuserIndex", .default_value = 0, .role = Role::EnumerationIndex};
inline const Field<std::int64_t> kQhyMaxStep{.key = "maxStep", .default_value = 64000, .min = 1, .max = 2000000};
inline const Field<bool> kQhyReverse{.key = "reverse", .default_value = false};
// 1 = fastest .. 8 = slowest.
inline const Field<std::int64_t> kQhySpeed{.key = "speed", .default_value = 1, .min = 1, .max = 8};
inline const Field<bool> kQhyHoldForce{.key = "holdForce", .default_value = false};
inline const Field<std::int64_t> kQhyHoldIhold{.key = "holdIhold", .default_value = 4, .min = 0, .max = 16};
inline const Field<std::int64_t> kQhyHoldIrun{.key = "holdIrun", .default_value = 8, .min = 0, .max = 30};
inline constexpr const char* kQhyTemperatureSources[] = {"external", "chip"};
inline const Field<std::string> kQhyTemperatureSource{
    .key = "temperatureSource", .default_value = "external", .allowed_values = kQhyTemperatureSources};

inline const std::vector<FieldRef>& qhy_camera_fields() {
    static const std::vector<FieldRef> fields{kQhyCameraIndex.ref(), kQhyCameraId.ref()};
    return fields;
}

inline const std::vector<FieldRef>& qhy_filterwheel_fields() {
    static const std::vector<FieldRef> fields{kQhyWheelType.ref(),     kQhyWheelConnectionType.ref(),
                                              kQhyWheelPortPath.ref(), kQhyWheelIndex.ref(),
                                              kQhyCameraIndex.ref(),   kQhyCameraId.ref(),
                                              kQhyFilterNames.ref()};
    return fields;
}

inline const std::vector<FieldRef>& qhy_focuser_fields() {
    static const std::vector<FieldRef> fields{kQhyFocuserConnectionType.ref(),
                                              kQhyFocuserPortPath.ref(),
                                              kQhyFocuserIndex.ref(),
                                              kQhyMaxStep.ref(),
                                              kQhyReverse.ref(),
                                              kQhySpeed.ref(),
                                              kQhyHoldForce.ref(),
                                              kQhyHoldIhold.ref(),
                                              kQhyHoldIrun.ref(),
                                              kQhyTemperatureSource.ref()};
    return fields;
}

// The cross-field refusals, in the router arms' order. The schema's normalize
// reports them (rejected from the API, warned about for a saved config), and the
// factory throws the same text so a saved config that normalize could only warn
// about is never built into a driver that would auto-probe instead (the CFW3
// probe DTR-resets every CP210x on the box).
inline std::optional<std::string> qhy_camera_handle_refusal(const DeviceConfig& config, const char* subject) {
    const auto index = config.find(kQhyCameraIndex);
    if (config.get(kQhyCameraId).empty() && !(index && *index >= 0)) {
        return std::string(subject) + " requires cameraIndex or cameraId";
    }
    return std::nullopt;
}

inline std::optional<std::string> qhy_camera_refusal(const DeviceConfig& config) {
    return qhy_camera_handle_refusal(config, "QHY camera");
}

inline std::optional<std::string> qhy_filterwheel_refusal(const DeviceConfig& config) {
    const std::string wheel_type = config.get(kQhyWheelType);
    if (wheel_type != "integrated" && wheel_type != "cfw3-usb") {
        return "QHY filter wheel wheelType must be \"integrated\" or \"cfw3-usb\"";
    }
    if (wheel_type == "integrated") return qhy_camera_handle_refusal(config, "QHY filter wheel");
    const std::string type = config.get(kQhyWheelConnectionType);
    if (type != "auto" && type != "serial") return "QHY CFW3 connectionType must be \"auto\" or \"serial\"";
    if (type == "serial") {
        if (config.get(kQhyWheelPortPath).empty()) return "QHY CFW3 connectionType \"serial\" requires portPath";
    } else if (config.get(kQhyWheelIndex) < 0) {
        return "QHY CFW3 filterwheelIndex must be 0 or greater";
    }
    return std::nullopt;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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

// ZWO catalog field declarations for the telescope (AM mounts), EFW filter
// wheel, EAF focuser, CAA rotator and the three switch backends (dew heater,
// ASIAIR Pro / Plus CM4, ASIAIR Plus RK3568), shared by zwo_schema.cpp (no
// vendor header) and zwo_catalog.cpp (the factory, vendor header allowed). No
// vendor header here either: the schema file includes this one and compiles in
// every build (including vendors-OFF). The camera stays in the router until its
// own slice.

#include <alpacacore/catalog/device_catalog.h>
#include <alpacacore/util/hardware_config_refusal.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Designated initializers of Field<T> leave most members defaulted on purpose
// (matches AlpacaHTTP/tests/test_catalog_descriptor.h).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

// Telescope. "serial", "network" and "auto" (scan at connect). No allowed_values:
// the per-field rule would substitute the default for an unknown saved value,
// which must be read as "serial" instead (#380), so the schema's normalize owns
// this rule. Unlike the other mounts an ABSENT connectionType is not "auto" here
// (the arm's valid list had no empty entry), so normalize reads it with find().
inline const Field<std::string> kZwoConnectionType{
    .key = "connectionType", .default_value = "auto", .role = Role::Discriminator};
inline const Field<std::string> kZwoPortPath{.key = "portPath",
                                             .default_value = "",
                                             .role = Role::PortPath,
                                             .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::int64_t> kZwoBaudRate{
    .key = "baudRate", .default_value = 9600, .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::string> kZwoHost{
    .key = "host", .default_value = "", .role = Role::Host, .applies_when = AppliesWhen{"connectionType", "network"}};
inline const Field<std::int64_t> kZwoTcpPort{
    .key = "tcpPort", .default_value = 4030, .applies_when = AppliesWhen{"connectionType", "network"}};
// Applied only when present: the driver's own default (5000 ms) holds otherwise.
inline const Field<std::int64_t> kZwoResponseTimeoutMs{.key = "responseTimeoutMs", .default_value = 5000};
// The 0.0 defaults below are not a site: the factory reads the site with find().
inline const Field<double> kZwoSiteLatitude{.key = "siteLatitude", .default_value = 0.0, .min = -90.0, .max = 90.0};
inline const Field<double> kZwoSiteLongitude{.key = "siteLongitude", .default_value = 0.0, .min = -180.0, .max = 180.0};
inline const Field<double> kZwoSiteElevation{.key = "siteElevation", .default_value = 0.0};
inline const Field<bool> kZwoSyncTimeOnConnect{.key = "syncTimeOnConnect", .default_value = false};
// Applied only when > 0, as the router arm did.
inline const Field<double> kZwoApertureDiameter{.key = "apertureDiameter", .default_value = 0.0};
inline const Field<double> kZwoFocalLength{.key = "focalLength", .default_value = 0.0};

// SDK-bound devices: an id (>= 0) wins over an index (>= 0), and one of the two is
// required. -1 means "not set", as the arm read it.
inline const Field<std::int64_t> kZwoFilterwheelIndex{
    .key = "filterwheelIndex", .default_value = -1, .role = Role::EnumerationIndex};
inline const Field<std::int64_t> kZwoFilterwheelId{.key = "filterwheelId", .default_value = -1, .role = Role::DeviceId};
inline const Field<std::vector<std::string>> kZwoFilterNames{.key = "filterNames", .default_value = {}};
inline const Field<std::int64_t> kZwoFocuserIndex{
    .key = "focuserIndex", .default_value = -1, .role = Role::EnumerationIndex};
inline const Field<std::int64_t> kZwoFocuserId{.key = "focuserId", .default_value = -1, .role = Role::DeviceId};
inline const Field<std::int64_t> kZwoRotatorIndex{
    .key = "rotatorIndex", .default_value = -1, .role = Role::EnumerationIndex};
inline const Field<std::int64_t> kZwoRotatorId{.key = "rotatorId", .default_value = -1, .role = Role::DeviceId};

// Switch. "dewheater" (a camera's heater through the camera SDK, the default),
// "asiair" (Pro, Pi 4), "asiair-plus-picm4" (Plus, Pi CM4; same wiring) and
// "asiair-plus-rk3568" (kernel module). normalize owns the enum rule so the
// refusal keeps its wording, and the match is case-insensitive as the arm's was.
inline const Field<std::string> kZwoSwitchType{
    .key = "switchType", .default_value = "dewheater", .role = Role::Discriminator};
inline const Field<std::int64_t> kZwoCameraIndex{.key = "cameraIndex",
                                                 .default_value = -1,
                                                 .role = Role::EnumerationIndex,
                                                 .applies_when = AppliesWhen{"switchType", "dewheater"}};
inline const Field<std::int64_t> kZwoCameraId{.key = "cameraId",
                                              .default_value = -1,
                                              .role = Role::DeviceId,
                                              .applies_when = AppliesWhen{"switchType", "dewheater"}};
inline const Field<std::string> kZwoGpioChip{
    .key = "gpioChip", .default_value = "/dev/gpiochip0", .applies_when = AppliesWhen{"switchType", "asiair"}};
inline const Field<std::string> kZwoDevicePath{.key = "devicePath",
                                               .default_value = "/dev/pwm-gpio-misc",
                                               .applies_when = AppliesWhen{"switchType", "asiair-plus-rk3568"}};
// Shown value is the Pro's; the factory reads it with find() so an unset value
// keeps each backend's own default (the RK3568 runs at 50 Hz).
inline const Field<std::int64_t> kZwoPwmFrequencyHz{.key = "pwmFrequencyHz", .default_value = 1000};

// One entry of ports[]. The Pro / CM4 layout requires an integer gpio, one of the
// board's four lines, each at most once; the RK3568 layout ignores gpio. A port is
// named "Port N" (its 1-based position) when no name is given.
inline const Field<std::int64_t> kZwoPortGpio{.key = "gpio", .default_value = 0};
inline const Field<std::string> kZwoPortName{.key = "name", .default_value = ""};
inline const Field<bool> kZwoPortPwm{.key = "pwm", .default_value = false};
inline const std::vector<FieldRef>& zwo_port_fields() {
    static const std::vector<FieldRef> fields{kZwoPortGpio.ref(), kZwoPortName.ref(), kZwoPortPwm.ref()};
    return fields;
}
inline const Field<std::vector<DeviceConfig>> kZwoPorts{
    .key = "ports", .default_value = {}, .record_fields = zwo_port_fields()};

inline const std::vector<FieldRef>& zwo_telescope_fields() {
    static const std::vector<FieldRef> fields{
        kZwoConnectionType.ref(), kZwoPortPath.ref(),          kZwoBaudRate.ref(),         kZwoHost.ref(),
        kZwoTcpPort.ref(),        kZwoResponseTimeoutMs.ref(), kZwoSiteLatitude.ref(),     kZwoSiteLongitude.ref(),
        kZwoSiteElevation.ref(),  kZwoSyncTimeOnConnect.ref(), kZwoApertureDiameter.ref(), kZwoFocalLength.ref()};
    return fields;
}

inline const std::vector<FieldRef>& zwo_filterwheel_fields() {
    static const std::vector<FieldRef> fields{kZwoFilterwheelIndex.ref(), kZwoFilterwheelId.ref(),
                                              kZwoFilterNames.ref()};
    return fields;
}

inline const std::vector<FieldRef>& zwo_focuser_fields() {
    static const std::vector<FieldRef> fields{kZwoFocuserIndex.ref(), kZwoFocuserId.ref()};
    return fields;
}

inline const std::vector<FieldRef>& zwo_rotator_fields() {
    static const std::vector<FieldRef> fields{kZwoRotatorIndex.ref(), kZwoRotatorId.ref()};
    return fields;
}

inline const std::vector<FieldRef>& zwo_switch_fields() {
    static const std::vector<FieldRef> fields{kZwoSwitchType.ref(), kZwoCameraIndex.ref(), kZwoCameraId.ref(),
                                              kZwoGpioChip.ref(),   kZwoDevicePath.ref(),  kZwoPwmFrequencyHz.ref(),
                                              kZwoPorts.ref()};
    return fields;
}

// The switchType in lower case, as the router arm compared it.
inline std::string zwo_switch_type(const DeviceConfig& config) {
    std::string type = config.get(kZwoSwitchType);
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });
    return type;
}

// An SDK-bound device needs an id or an index (>= 0), else `message`.
inline std::optional<std::string> zwo_bound_refusal(const DeviceConfig& config, const Field<std::int64_t>& id,
                                                    const Field<std::int64_t>& index, const char* message) {
    if (config.get(id) >= 0 || config.get(index) >= 0) return std::nullopt;
    return std::string(message);
}

// The switch's cross-field refusals, in the router arm's order. normalize reports
// them (rejected from the API, warned about for a saved config) and the factory
// throws the same text, so a saved config normalize could only warn about is
// never built into a device.
inline std::optional<std::string> zwo_switch_refusal(const DeviceConfig& config) {
    const std::string type = zwo_switch_type(config);
    if (type != "dewheater" && type != "asiair" && type != "asiair-plus-picm4" && type != "asiair-plus-rk3568") {
        return "ZWO switchType must be 'dewheater', 'asiair', 'asiair-plus-picm4', or 'asiair-plus-rk3568'";
    }
    if (type == "asiair-plus-rk3568") {
        if (config.get(kZwoDevicePath) != "/dev/pwm-gpio-misc") {
            return util::hardware_config_refusal("devicePath", "'/dev/pwm-gpio-misc'");
        }
        return std::nullopt;
    }
    if (type == "asiair" || type == "asiair-plus-picm4") {
        if (config.get(kZwoGpioChip) != "/dev/gpiochip0") {
            return util::hardware_config_refusal("gpioChip", "'/dev/gpiochip0'");
        }
        if (const auto ports = config.find(kZwoPorts)) {
            std::set<std::int64_t> seen;
            for (const DeviceConfig& port : *ports) {
                const auto gpio = port.find(kZwoPortGpio);
                if (!gpio) return "ASIAIR port entry requires integer 'gpio'";
                if (*gpio != 12 && *gpio != 13 && *gpio != 26 && *gpio != 18) {
                    return util::hardware_config_refusal("ports[].gpio", "one of 12, 13, 26, 18");
                }
                if (!seen.insert(*gpio).second) {
                    return util::hardware_config_refusal("ports[].gpio", "each of 12, 13, 26, 18 at most once");
                }
            }
        }
        return std::nullopt;
    }
    return zwo_bound_refusal(config, kZwoCameraId, kZwoCameraIndex,
                             "ZWO dew heater switch requires cameraIndex or cameraId");
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

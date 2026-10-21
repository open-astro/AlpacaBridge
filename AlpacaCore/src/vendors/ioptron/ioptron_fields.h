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

// iOptron catalog field declarations (telescope, iMate PowerBox switch, iEAF
// focuser, iEFW filter wheel, iGuider guide camera, iCAM camera), shared by ioptron_schema.cpp (no
// vendor header) and ioptron_catalog.cpp (the factories, vendor header
// allowed). No vendor header here either: the schema file includes this one and
// compiles in every build (including vendors-OFF).

#include <alpacacore/catalog/device_catalog.h>
#include <alpacacore/util/hardware_config_refusal.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ioptron_camera_fields.h"

// Designated initializers of Field<T> leave most members defaulted on purpose
// (matches AlpacaHTTP/tests/test_catalog_descriptor.h).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

// ---- Telescope (serial or network, or auto-detect) ------------------------

// "auto" (or unset) scans at connect; "serial" names the port; "network" names
// the host, or auto-detects on the subnet when the host is empty.
inline const Field<std::string> kIoptronConnectionType{
    .key = "connectionType", .default_value = "auto", .role = Role::Discriminator};
inline const Field<std::int64_t> kIoptronMountIndex{
    .key = "mountIndex", .default_value = 0, .role = Role::EnumerationIndex};
inline const Field<std::string> kIoptronPortPath{.key = "portPath",
                                                 .default_value = "",
                                                 .role = Role::PortPath,
                                                 .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::int64_t> kIoptronBaudRate{
    .key = "baudRate", .default_value = 115200, .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::string> kIoptronHost{
    .key = "host", .default_value = "", .role = Role::Host, .applies_when = AppliesWhen{"connectionType", "network"}};
inline const Field<std::int64_t> kIoptronTcpPort{
    .key = "tcpPort", .default_value = 4030, .applies_when = AppliesWhen{"connectionType", "network"}};
inline const Field<std::int64_t> kIoptronResponseTimeoutMs{.key = "responseTimeoutMs", .default_value = 5000};
// No min/max on the site fields: normalize words the range refusal itself (the
// text the router's former site check gave, kept), and an out-of-range saved value
// is dropped there.
inline const Field<double> kIoptronSiteLatitude{.key = "siteLatitude", .default_value = 0.0};
inline const Field<double> kIoptronSiteLongitude{.key = "siteLongitude", .default_value = 0.0};
inline const Field<double> kIoptronSiteElevation{.key = "siteElevation", .default_value = 0.0};
inline const Field<bool> kIoptronSyncTimeOnConnect{.key = "syncTimeOnConnect", .default_value = false};
inline const Field<double> kIoptronApertureDiameter{.key = "apertureDiameter", .default_value = 0.0};
inline const Field<double> kIoptronFocalLength{.key = "focalLength", .default_value = 0.0};

inline const std::vector<FieldRef>& ioptron_telescope_fields() {
    static const std::vector<FieldRef> fields{kIoptronConnectionType.ref(),
                                              kIoptronMountIndex.ref(),
                                              kIoptronPortPath.ref(),
                                              kIoptronBaudRate.ref(),
                                              kIoptronHost.ref(),
                                              kIoptronTcpPort.ref(),
                                              kIoptronResponseTimeoutMs.ref(),
                                              kIoptronSiteLatitude.ref(),
                                              kIoptronSiteLongitude.ref(),
                                              kIoptronSiteElevation.ref(),
                                              kIoptronSyncTimeOnConnect.ref(),
                                              kIoptronApertureDiameter.ref(),
                                              kIoptronFocalLength.ref()};
    return fields;
}

// ---- iEAF / iAFS focuser (USB-serial, fixed baud) -------------------------

inline const Field<std::int64_t> kIoptronFocuserIndex{
    .key = "focuserIndex", .default_value = 0, .role = Role::EnumerationIndex};
// "ieaf" (default) or "iafs2": identical protocol, sets the reported device name.
inline const Field<std::string> kIoptronFocuserModel{.key = "model", .default_value = "ieaf"};

inline const std::vector<FieldRef>& ioptron_focuser_fields() {
    static const std::vector<FieldRef> fields{kIoptronConnectionType.ref(), kIoptronPortPath.ref(),
                                              kIoptronFocuserIndex.ref(), kIoptronFocuserModel.ref()};
    return fields;
}

// ---- iEFW filter wheel (USB-serial, fixed baud) ---------------------------

inline const Field<std::int64_t> kIoptronFilterwheelIndex{
    .key = "filterwheelIndex", .default_value = 0, .role = Role::EnumerationIndex};
// "iefw15" (default) or "iefw18": the reported device name; the slot count comes
// from the wheel's handshake.
inline const Field<std::string> kIoptronFilterwheelModel{.key = "model", .default_value = "iefw15"};
// Absent leaves the driver's own names; a non-array or non-string element is
// refused by the declared StringList kind.
inline const Field<std::vector<std::string>> kIoptronFilterNames{.key = "filterNames", .default_value = {}};

inline const std::vector<FieldRef>& ioptron_filterwheel_fields() {
    static const std::vector<FieldRef> fields{kIoptronConnectionType.ref(), kIoptronPortPath.ref(),
                                              kIoptronFilterwheelIndex.ref(), kIoptronFilterwheelModel.ref(),
                                              kIoptronFilterNames.ref()};
    return fields;
}

inline constexpr const char* kIoptronSerialPortRequired =
    "portPath is required when connectionType is 'serial' (or use 'auto').";

// The wheel's refusals, in the router arm's order. normalize reports them and the
// factory throws the same text, so a saved entry normalize can only warn about is
// still not built (it stays dropped, as before).
inline std::optional<std::string> ioptron_filterwheel_refusal(const DeviceConfig& config) {
    if (config.get(kIoptronConnectionType) == "serial") {
        if (config.get(kIoptronPortPath).empty()) return kIoptronSerialPortRequired;
    } else if (config.get(kIoptronFilterwheelIndex) < 0) {
        return "filterwheelIndex must be >= 0.";
    }
    return std::nullopt;
}

// ---- iGuider guide camera (UVC over V4L2) ---------------------------------

// Empty finds the first iGuider at connect; otherwise the V4L2 node to open
// (a /dev/v4l/by-id link survives a replug, /dev/videoN may not).
inline const Field<std::string> kIguiderDevicePath{.key = "devicePath", .default_value = "", .role = Role::PortPath};

inline const std::vector<FieldRef>& iguider_camera_fields() {
    static const std::vector<FieldRef> fields{kIguiderDevicePath.ref()};
    return fields;
}

// A config may name only a V4L2 capture node (decision 0007, rule 6): the driver
// opens the path read-write and issues V4L2 ioctls on it.
inline std::optional<std::string> iguider_camera_refusal(const DeviceConfig& config) {
    const std::string path = config.get(kIguiderDevicePath);
    if (path.empty()) return std::nullopt;
    const bool v4l2_node = path.rfind("/dev/video", 0) == 0 || path.rfind("/dev/v4l/", 0) == 0;
    if (!v4l2_node || path.size() > 256 || path.find("..") != std::string::npos) {
        return "devicePath must be empty (auto-detect) or a /dev/video* or /dev/v4l/... capture node.";
    }
    return std::nullopt;
}

// ---- iMate PowerBox switch (local GPIO, libgpiod) -------------------------

inline const Field<std::string> kIoptronGpioChip{.key = "gpioChip", .default_value = "/dev/gpiochip1"};
// The value shown is the driver's own default; the factory reads it with find().
inline const Field<std::int64_t> kIoptronPwmFrequencyHz{.key = "pwmFrequencyHz", .default_value = 50};

// One entry of ports[], applied by position onto the fixed DC3/DC1/DC2 layout.
// The always-on pass-through has no GPIO line, so its pwm flag is ignored.
inline const Field<std::string> kIoptronPortName{.key = "name", .default_value = ""};
inline const Field<bool> kIoptronPortPwm{.key = "pwm", .default_value = false};
inline const std::vector<FieldRef>& ioptron_port_fields() {
    static const std::vector<FieldRef> fields{kIoptronPortName.ref(), kIoptronPortPwm.ref()};
    return fields;
}
inline const Field<std::vector<DeviceConfig>> kIoptronPorts{
    .key = "ports", .default_value = {}, .record_fields = ioptron_port_fields()};

inline const std::vector<FieldRef>& ioptron_switch_fields() {
    static const std::vector<FieldRef> fields{kIoptronGpioChip.ref(), kIoptronPwmFrequencyHz.ref(),
                                              kIoptronPorts.ref()};
    return fields;
}

// The board's GPIO chip only (#765): a config may not choose which chip the server
// opens. The check exists only where the PowerBox is built
// (ALPACACORE_IOPTRON_POWERBOX, defined for the builtins library), as the router
// arm only ran it there.
inline std::optional<std::string> ioptron_switch_refusal([[maybe_unused]] const DeviceConfig& config) {
#ifdef ALPACACORE_IOPTRON_POWERBOX
    const std::string chip = config.get(kIoptronGpioChip);
    // /dev/gpiochip0 is the stock BSP kernel's name for the same bank.
    if (chip != "/dev/gpiochip1" && chip != "/dev/gpiochip0") {
        return util::hardware_config_refusal("gpioChip", "'/dev/gpiochip1' or '/dev/gpiochip0'");
    }
#endif
    return std::nullopt;
}

// ---- Telescope normalize ---------------------------------------------------

// The wording of the router's former site check, kept: "siteLatitude 200 is out of
// range: must be between -90 and 90 degrees". NaN fails the in-range test too.
inline std::optional<std::string> ioptron_site_refusal(const Field<double>& field, const DeviceConfig& config,
                                                       double limit) {
    const std::optional<double> value = config.find(field);
    if (!value || (*value >= -limit && *value <= limit)) return std::nullopt;
    const auto kDouble = FieldRef::Kind::Double;
    return std::string(field.key) + " " + format_bound(kDouble, *value) + " is out of range: must be between " +
           format_bound(kDouble, -limit) + " and " + format_bound(kDouble, limit) + " degrees";
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

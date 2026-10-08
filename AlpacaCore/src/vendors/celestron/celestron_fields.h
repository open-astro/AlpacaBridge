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

// Celestron telescope catalog field declarations, shared by celestron_schema.cpp
// (no vendor header) and celestron_catalog.cpp (the factory, vendor header
// allowed). No vendor header here either: the schema file includes this one and
// compiles in every build (including vendors-OFF).

#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <string>
#include <vector>

// Designated initializers of Field<T> leave most members defaulted on purpose
// (matches AlpacaHTTP/tests/test_catalog_descriptor.h).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

// "" and "auto" scan at connect; "serial" and "network" name the endpoint. No
// allowed_values: the per-field rule would substitute the default "auto" for an
// unknown saved value, which must be read as "serial" instead (#380), so the
// schema's normalize owns this rule.
inline const Field<std::string> kCelestronConnectionType{
    .key = "connectionType", .default_value = "auto", .role = Role::Discriminator};
inline const Field<std::int64_t> kCelestronMountIndex{
    .key = "mountIndex", .default_value = 0, .role = Role::EnumerationIndex};
inline const Field<std::string> kCelestronPortPath{.key = "portPath",
                                                   .default_value = "",
                                                   .role = Role::PortPath,
                                                   .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::int64_t> kCelestronBaudRate{
    .key = "baudRate", .default_value = 9600, .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::string> kCelestronHost{
    .key = "host", .default_value = "", .role = Role::Host, .applies_when = AppliesWhen{"connectionType", "network"}};
inline const Field<std::int64_t> kCelestronTcpPort{
    .key = "tcpPort", .default_value = 2000, .applies_when = AppliesWhen{"connectionType", "network"}};
inline const Field<std::int64_t> kCelestronResponseTimeoutMs{.key = "responseTimeoutMs", .default_value = 5000};
// #860: "auto", "altaz" or "equatorial". No allowed_values: the per-field rule
// would substitute "auto" for an unknown string, and an unknown value must drop
// instead, so the schema's normalize owns this rule too.
inline const Field<std::string> kCelestronAlignmentMode{.key = "alignmentMode", .default_value = "auto"};
// The 0.0 defaults below are not a site: the factory reads the site with
// find(), and an unset coordinate stays unset.
inline const Field<double> kCelestronSiteLatitude{
    .key = "siteLatitude", .default_value = 0.0, .min = -90.0, .max = 90.0};
inline const Field<double> kCelestronSiteLongitude{
    .key = "siteLongitude", .default_value = 0.0, .min = -180.0, .max = 180.0};
inline const Field<double> kCelestronSiteElevation{.key = "siteElevation", .default_value = 0.0};
inline const Field<bool> kCelestronSyncTimeOnConnect{.key = "syncTimeOnConnect", .default_value = false};
// Applied only when > 0, as the router arm did.
inline const Field<double> kCelestronApertureDiameter{.key = "apertureDiameter", .default_value = 0.0};
inline const Field<double> kCelestronFocalLength{.key = "focalLength", .default_value = 0.0};

inline const std::vector<FieldRef>& celestron_telescope_fields() {
    static const std::vector<FieldRef> fields{
        kCelestronConnectionType.ref(),    kCelestronMountIndex.ref(),    kCelestronPortPath.ref(),
        kCelestronBaudRate.ref(),          kCelestronHost.ref(),          kCelestronTcpPort.ref(),
        kCelestronResponseTimeoutMs.ref(), kCelestronAlignmentMode.ref(), kCelestronSiteLatitude.ref(),
        kCelestronSiteLongitude.ref(),     kCelestronSiteElevation.ref(), kCelestronSyncTimeOnConnect.ref(),
        kCelestronApertureDiameter.ref(),  kCelestronFocalLength.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

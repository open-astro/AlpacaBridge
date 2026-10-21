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

// SynScan telescope catalog field declarations, shared by synscan_schema.cpp
// (no vendor header) and synscan_catalog.cpp (the factory, vendor header
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
inline const Field<std::string> kSynScanConnectionType{
    .key = "connectionType", .default_value = "auto", .role = Role::Discriminator};
inline const Field<std::int64_t> kSynScanMountIndex{
    .key = "mountIndex", .default_value = 0, .role = Role::EnumerationIndex};
// "auto", "v3" or "v4" (the factory also reads "3"/"4" and any case, as the router
// arm did). A free string: an unknown value reads as "auto".
inline const Field<std::string> kSynScanVersion{.key = "synscanVersion", .default_value = "auto"};
inline const Field<std::string> kSynScanPortPath{.key = "portPath",
                                                 .default_value = "",
                                                 .role = Role::PortPath,
                                                 .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::int64_t> kSynScanBaudRate{
    .key = "baudRate", .default_value = 9600, .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::string> kSynScanHost{
    .key = "host", .default_value = "", .role = Role::Host, .applies_when = AppliesWhen{"connectionType", "network"}};
inline const Field<std::int64_t> kSynScanTcpPort{
    .key = "tcpPort", .default_value = 11880, .applies_when = AppliesWhen{"connectionType", "network"}};
inline const Field<std::int64_t> kSynScanResponseTimeoutMs{.key = "responseTimeoutMs", .default_value = 5000};
// #860: "auto", "altaz" or "equatorial". No allowed_values: the per-field rule
// would substitute "auto" for an unknown string, and an unknown value must drop
// instead, so the schema's normalize owns this rule too.
inline const Field<std::string> kSynScanAlignmentMode{.key = "alignmentMode", .default_value = "auto"};
// The 0.0 defaults below are not a site: the factory reads the site with
// find(), and an unset coordinate stays unset.
inline const Field<double> kSynScanSiteLatitude{.key = "siteLatitude", .default_value = 0.0, .min = -90.0, .max = 90.0};
inline const Field<double> kSynScanSiteLongitude{
    .key = "siteLongitude", .default_value = 0.0, .min = -180.0, .max = 180.0};
inline const Field<double> kSynScanSiteElevation{.key = "siteElevation", .default_value = 0.0};
inline const Field<bool> kSynScanSyncTimeOnConnect{.key = "syncTimeOnConnect", .default_value = false};
// Applied only when > 0, as the router arm did.
inline const Field<double> kSynScanApertureDiameter{.key = "apertureDiameter", .default_value = 0.0};
inline const Field<double> kSynScanFocalLength{.key = "focalLength", .default_value = 0.0};

inline const std::vector<FieldRef>& synscan_telescope_fields() {
    static const std::vector<FieldRef> fields{
        kSynScanConnectionType.ref(),    kSynScanMountIndex.ref(),        kSynScanVersion.ref(),
        kSynScanPortPath.ref(),          kSynScanBaudRate.ref(),          kSynScanHost.ref(),
        kSynScanTcpPort.ref(),           kSynScanResponseTimeoutMs.ref(), kSynScanAlignmentMode.ref(),
        kSynScanSiteLatitude.ref(),      kSynScanSiteLongitude.ref(),     kSynScanSiteElevation.ref(),
        kSynScanSyncTimeOnConnect.ref(), kSynScanApertureDiameter.ref(),  kSynScanFocalLength.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

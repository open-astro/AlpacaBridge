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

// Sky-Watcher direct (motor controller) telescope catalog field declarations
// (open-astro#744), shared by skywatcher_schema.cpp (no vendor header) and
// skywatcher_catalog.cpp (the factory, vendor header allowed). No vendor
// header here either: the schema file includes this one and compiles in every
// build (including vendors-OFF), and the layering gate scans only the schema
// file, not its includes.

#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Designated initializers of Field<T> leave most members defaulted on purpose
// (matches AlpacaHTTP/tests/test_catalog_descriptor.h).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

// #274: the mount stores no site of its own. Returned to the API by the
// schema's normalize and appended to the factory's WARNING for a saved config.
inline constexpr std::string_view kSkyWatcherMissingSite =
    "Site latitude and longitude are required for the Sky-Watcher direct driver: this mount stores no site of its "
    "own, and tracking direction, guide sign and pier side are all hemisphere-dependent";

// "" and "auto" scan at connect; "serial" and "network" name the endpoint. No
// allowed_values: the per-field rule would substitute the default "auto" for
// an unknown saved value, which must be read as "serial" instead (#380), so
// the schema's normalize owns this rule.
inline const Field<std::string> kSkyWatcherConnectionType{
    .key = "connectionType", .default_value = "auto", .role = Role::Discriminator};
inline const Field<std::int64_t> kSkyWatcherMountIndex{
    .key = "mountIndex", .default_value = 0, .role = Role::EnumerationIndex};
inline const Field<std::string> kSkyWatcherPortPath{.key = "portPath",
                                                    .default_value = "",
                                                    .role = Role::PortPath,
                                                    .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::int64_t> kSkyWatcherBaudRate{
    .key = "baudRate", .default_value = 9600, .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::string> kSkyWatcherHost{
    .key = "host", .default_value = "", .role = Role::Host, .applies_when = AppliesWhen{"connectionType", "network"}};
inline const Field<std::int64_t> kSkyWatcherUdpPort{
    .key = "udpPort", .default_value = 11880, .applies_when = AppliesWhen{"connectionType", "network"}};
inline const Field<std::int64_t> kSkyWatcherResponseTimeoutMs{.key = "responseTimeoutMs", .default_value = 1000};
// The 0.0 defaults below are not a site: the factory reads these with find(),
// and an unset coordinate stays unset (#274).
inline const Field<double> kSkyWatcherSiteLatitude{
    .key = "siteLatitude", .default_value = 0.0, .min = -90.0, .max = 90.0};
inline const Field<double> kSkyWatcherSiteLongitude{
    .key = "siteLongitude", .default_value = 0.0, .min = -180.0, .max = 180.0};
inline const Field<double> kSkyWatcherSiteElevation{.key = "siteElevation", .default_value = 0.0};
// Applied only when > 0, as the router arm did.
inline const Field<double> kSkyWatcherApertureDiameter{.key = "apertureDiameter", .default_value = 0.0};
inline const Field<double> kSkyWatcherFocalLength{.key = "focalLength", .default_value = 0.0};

inline const std::vector<FieldRef>& skywatcher_telescope_fields() {
    static const std::vector<FieldRef> fields{kSkyWatcherConnectionType.ref(),
                                              kSkyWatcherMountIndex.ref(),
                                              kSkyWatcherPortPath.ref(),
                                              kSkyWatcherBaudRate.ref(),
                                              kSkyWatcherHost.ref(),
                                              kSkyWatcherUdpPort.ref(),
                                              kSkyWatcherResponseTimeoutMs.ref(),
                                              kSkyWatcherSiteLatitude.ref(),
                                              kSkyWatcherSiteLongitude.ref(),
                                              kSkyWatcherSiteElevation.ref(),
                                              kSkyWatcherApertureDiameter.ref(),
                                              kSkyWatcherFocalLength.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

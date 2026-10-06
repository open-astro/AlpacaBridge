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

// Bisque / Paramount (TheSkyX) telescope catalog field declarations, shared
// by bisque_schema.cpp (no vendor header) and bisque_catalog.cpp (the factory,
// vendor header allowed). No vendor header here either: the schema file
// includes this one and compiles in every build (including vendors-OFF).

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

// TCP only: no connectionType. The defaults are the router arm's.
inline const Field<std::string> kBisqueHost{.key = "host", .default_value = "localhost", .role = Role::Host};
inline const Field<std::int64_t> kBisqueTcpPort{.key = "tcpPort", .default_value = 3040};
inline const Field<std::int64_t> kBisqueResponseTimeoutMs{.key = "responseTimeoutMs", .default_value = 3000};
// The 0.0 defaults below are not a site: the factory reads the site with
// find(), and an unset coordinate stays unset.
inline const Field<double> kBisqueSiteLatitude{.key = "siteLatitude", .default_value = 0.0, .min = -90.0, .max = 90.0};
inline const Field<double> kBisqueSiteLongitude{
    .key = "siteLongitude", .default_value = 0.0, .min = -180.0, .max = 180.0};
inline const Field<double> kBisqueSiteElevation{.key = "siteElevation", .default_value = 0.0};
// Applied only when > 0, as the router arm did.
inline const Field<double> kBisqueApertureDiameter{.key = "apertureDiameter", .default_value = 0.0};
inline const Field<double> kBisqueFocalLength{.key = "focalLength", .default_value = 0.0};

inline const std::vector<FieldRef>& bisque_telescope_fields() {
    static const std::vector<FieldRef> fields{kBisqueHost.ref(),
                                              kBisqueTcpPort.ref(),
                                              kBisqueResponseTimeoutMs.ref(),
                                              kBisqueSiteLatitude.ref(),
                                              kBisqueSiteLongitude.ref(),
                                              kBisqueSiteElevation.ref(),
                                              kBisqueApertureDiameter.ref(),
                                              kBisqueFocalLength.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

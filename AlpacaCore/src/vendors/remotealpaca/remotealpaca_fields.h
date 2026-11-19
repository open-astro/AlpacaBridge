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

// remotealpaca ObservingConditions catalog field declarations (decision 0023, issue #1753 slice 3), shared by
// remotealpaca_schema.cpp (no vendor header) and remotealpaca_catalog.cpp (the factory, vendor header allowed). No
// vendor header here either: the schema file includes this one and compiles in every build (including vendors-OFF).
// The keys say "upstream" so none collides with the router's own deviceNumber. No field declares a required, min or
// max rule: Source::Persisted only warns on one and registers the device, so the factory refuses a missing host and
// each value out of range (decision 0007 rule 4) from both config sources, as the WeeWX factory does.

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

// The upstream Alpaca server: a host name or an IP address, no scheme and no brackets.
inline const Field<std::string> kRemoteAlpacaHost{.key = "upstreamHost", .default_value = "", .role = Role::Host};
// Its HTTP port, 1 to 65535. 32323 is the ASCOM Alpaca Simulators' default.
inline const Field<std::int64_t> kRemoteAlpacaPort{.key = "upstreamPort", .default_value = 32323};
// The upstream's own ObservingConditions device number, not this server's.
inline const Field<std::int64_t> kRemoteAlpacaDeviceNumber{.key = "upstreamDeviceNumber", .default_value = 0};
// The poll period, 1 to 60 seconds (#1753 maintainer decision, 2026-11-16).
inline const Field<std::int64_t> kRemoteAlpacaPollSeconds{.key = "pollPeriodSeconds", .default_value = 2};

inline const std::vector<FieldRef>& remotealpaca_observingconditions_fields() {
    static const std::vector<FieldRef> fields{kRemoteAlpacaHost.ref(), kRemoteAlpacaPort.ref(),
                                              kRemoteAlpacaDeviceNumber.ref(), kRemoteAlpacaPollSeconds.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

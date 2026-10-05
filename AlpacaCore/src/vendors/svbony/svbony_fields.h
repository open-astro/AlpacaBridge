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

// SVBONY camera catalog field declarations, shared by svbony_schema.cpp (no
// vendor header) and svbony_catalog.cpp (the factory, vendor header allowed).
// No vendor header here either: the schema file includes this one and compiles
// in every build (including vendors-OFF).

#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <vector>

// Designated initializers of Field<T> leave most members defaulted on purpose
// (matches AlpacaHTTP/tests/test_catalog_descriptor.h).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

// 0-based SVBONY SDK camera enumeration index. No declared range: the router
// arm this replaces passed any value through, and the driver refuses an
// out-of-range index at connect.
inline const Field<std::int64_t> kSvbonyCameraIndex{
    .key = "cameraIndex", .default_value = 0, .role = Role::EnumerationIndex};

inline const std::vector<FieldRef>& svbony_camera_fields() {
    static const std::vector<FieldRef> fields{kSvbonyCameraIndex.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

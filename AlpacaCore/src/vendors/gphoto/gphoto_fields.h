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

// gphoto camera catalog field declarations, shared by gphoto_schema.cpp (no
// vendor header) and gphoto_catalog.cpp (the factory, vendor header allowed).
// No vendor header here either: the schema file includes this one and compiles
// in every build (including vendors-OFF).

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

// 0-based libgphoto2 camera enumeration index. No declared range: the router
// arm this replaces passed any value through, and the driver refuses an
// out-of-range index at connect.
inline const Field<std::int64_t> kGphotoCameraIndex{
    .key = "cameraIndex", .default_value = 0, .role = Role::EnumerationIndex};

// How long the mirror stays up between the two shutter presses a body with mirror lock-up (MLU) on needs. The
// driver follows the camera's own MLU menu setting and never writes it; this is only the settle between the
// presses. Unused while MLU is off.
inline const Field<std::int64_t> kGphotoMirrorLockupSettleMs{
    .key = "mirrorLockupSettleMs", .default_value = 2000, .min = 500, .max = 10000};

// Long-exposure noise reduction. "camera" writes nothing and leaves the camera's own setting; "off" and "on"
// write the Nikon longexpnr widget once at connect. A body without the widget (every Canon tested) logs a
// warning and carries on.
inline constexpr const char* kGphotoLongExposureNrModes[] = {"camera", "off", "on"};
inline const Field<std::string> kGphotoLongExposureNr{
    .key = "longExposureNR", .default_value = "camera", .allowed_values = kGphotoLongExposureNrModes};

inline const std::vector<FieldRef>& gphoto_camera_fields() {
    static const std::vector<FieldRef> fields{kGphotoCameraIndex.ref(), kGphotoMirrorLockupSettleMs.ref(),
                                              kGphotoLongExposureNr.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

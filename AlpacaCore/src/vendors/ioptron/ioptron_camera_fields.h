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

// The iOptron iCAM camera's field declaration, in a header of its own: it is built
// by the Player One factory (playerone_catalog.cpp), which must not see the rest of
// ioptron_fields.h (that file's inline refusals read ALPACACORE_IOPTRON_POWERBOX,
// which differs between the translation units that include it). No vendor header.

#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

// ---- iCAM camera (a rebadged Player One) ----------------------------------

// The Player One SDK enumeration index, with no range: the arm it replaces never
// validated it, and the driver refuses an index past the attached cameras at connect.
inline const Field<std::int64_t> kIoptronCameraIndex{
    .key = "cameraIndex", .default_value = 0, .role = Role::EnumerationIndex};

inline const std::vector<FieldRef>& ioptron_camera_fields() {
    static const std::vector<FieldRef> fields{kIoptronCameraIndex.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

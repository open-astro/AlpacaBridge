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

// The Altair camera schema. No vendor header: compiles in every build
// (including vendors-OFF), so `available` in GET /management/v1/devicecatalog
// reflects only whether a factory is registered.

#include "../../catalog/builtin_descriptors.h"
#include "altair_fields.h"

namespace alpacacore::catalog {

void register_altair_schema(DeviceCatalog& catalog) {
    Schema schema;
    schema.key = DeviceKey{"altair", DeviceType::Camera};
    schema.display_name = "Altair Camera";
    schema.build_option = "ALPACACORE_ENABLE_ALTAIR";
    schema.fields = altair_camera_fields();
    catalog.add(std::move(schema));
}

}  // namespace alpacacore::catalog

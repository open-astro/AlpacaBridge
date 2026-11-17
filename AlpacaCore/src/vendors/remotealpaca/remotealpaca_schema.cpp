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

// The remotealpaca ObservingConditions schema (decision 0023, issue #1753 slice 3). No vendor header: compiles in
// every build (including vendors-OFF), so `available` in GET /management/v1/devicecatalog reflects only whether a
// factory is registered. No per-field rule: the refusals live in the factory (see remotealpaca_fields.h).

#include "../../catalog/builtin_descriptors.h"
#include "remotealpaca_fields.h"

namespace alpacacore::catalog {

void register_remotealpaca_schema(DeviceCatalog& catalog) {
    Schema schema;
    schema.key = DeviceKey{"remotealpaca", DeviceType::ObservingConditions};
    schema.display_name = "Remote Alpaca Observing Conditions";
    schema.build_option = "ALPACACORE_ENABLE_REMOTEALPACA";
    schema.vendor_label = "Remote Alpaca";
    schema.fields = remotealpaca_observingconditions_fields();
    catalog.add(std::move(schema));
}

}  // namespace alpacacore::catalog

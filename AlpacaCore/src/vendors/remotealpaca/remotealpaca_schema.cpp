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

// The remotealpaca schemas, ObservingConditions, SafetyMonitor and Rotator (decision 0023, issue #1753 slices 3 to 5).
// No vendor header: compiles in every build (including vendors-OFF), so `available` in GET
// /management/v1/devicecatalog reflects only whether a factory is registered. No per-field rule: the refusals live in
// the factory (see remotealpaca_fields.h).

#include <string_view>
#include <utility>

#include "../../catalog/builtin_descriptors.h"
#include "remotealpaca_fields.h"

namespace alpacacore::catalog {

void register_remotealpaca_schema(DeviceCatalog& catalog) {
    const auto add = [&catalog](DeviceType type, std::string_view display_name) {
        Schema schema;
        schema.key = DeviceKey{"remotealpaca", type};
        schema.display_name = display_name;
        schema.build_option = "ALPACACORE_ENABLE_REMOTEALPACA";
        schema.vendor_label = "Remote Alpaca";
        schema.fields = remotealpaca_device_fields();
        catalog.add(std::move(schema));
    };
    add(DeviceType::ObservingConditions, "Remote Alpaca Observing Conditions");
    add(DeviceType::SafetyMonitor, "Remote Alpaca Safety Monitor");
    add(DeviceType::Rotator, "Remote Alpaca Rotator");
}

}  // namespace alpacacore::catalog

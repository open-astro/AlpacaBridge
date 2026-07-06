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

#include <alpacacore/device_capabilities.h>
#include <alpacacore/device_registry.h>
#include <alpacacore/util/logging.h>
#include <vector>

namespace alpacacore {

std::vector<DeviceCapabilities> get_all_device_capabilities() {
    return management::DeviceRegistry::instance().get_all_device_capabilities();
}

} // namespace alpacacore


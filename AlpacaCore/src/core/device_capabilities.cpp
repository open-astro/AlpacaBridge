// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#include <alpacacore/device_capabilities.h>
#include <alpacacore/device_registry.h>
#include <alpacacore/util/logging.h>
#include <vector>

namespace alpacacore {

std::vector<DeviceCapabilities> get_all_device_capabilities() {
    return management::DeviceRegistry::instance().get_all_device_capabilities();
}

} // namespace alpacacore


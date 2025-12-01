// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://www.mongodb.com/licensing/server-side-public-license
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

#include <alpacacore/device_capabilities.h>
#include <alpacacore/util/logging.h>
#include <vector>

namespace alpacacore {

std::vector<DeviceCapabilities> get_all_device_capabilities() {
    // TODO: Implement device registry lookup
    ALPACA_LOG_INFO("DeviceCapabilities", "get_all_device_capabilities called");
    return {};
}

} // namespace alpacacore


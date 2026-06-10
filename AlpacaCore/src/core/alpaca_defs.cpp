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

#include <alpacacore/alpaca_defs.h>

namespace alpacacore {

const char* device_type_to_string(DeviceType type) {
    switch (type) {
        case DeviceType::Camera:            return "Camera";
        case DeviceType::Telescope:         return "Telescope";
        case DeviceType::FilterWheel:       return "FilterWheel";
        case DeviceType::Focuser:           return "Focuser";
        case DeviceType::Rotator:           return "Rotator";
        case DeviceType::Dome:
            return "Dome";
        case DeviceType::Switch:             return "Switch";
        case DeviceType::CoverCalibrator:    return "CoverCalibrator";
        case DeviceType::ObservingConditions: return "ObservingConditions";
        case DeviceType::SafetyMonitor:      return "SafetyMonitor";
        default:                            return "Unknown";
    }
}

} // namespace alpacacore


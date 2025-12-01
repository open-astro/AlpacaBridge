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

#include <alpacacore/alpaca_defs.h>

namespace alpacacore {

const char* device_type_to_string(DeviceType type) {
    switch (type) {
        case DeviceType::Camera:            return "Camera";
        case DeviceType::Telescope:         return "Telescope";
        case DeviceType::FilterWheel:       return "FilterWheel";
        case DeviceType::Focuser:           return "Focuser";
        case DeviceType::Rotator:           return "Rotator";
        case DeviceType::Dome:              return "Dome";
        case DeviceType::Shutter:           return "Shutter";
        case DeviceType::Switch:             return "Switch";
        case DeviceType::CoverCalibrator:    return "CoverCalibrator";
        case DeviceType::ObservingConditions: return "ObservingConditions";
        case DeviceType::SafetyMonitor:      return "SafetyMonitor";
        default:                            return "Unknown";
    }
}

} // namespace alpacacore


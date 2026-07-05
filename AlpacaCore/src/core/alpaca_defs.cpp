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


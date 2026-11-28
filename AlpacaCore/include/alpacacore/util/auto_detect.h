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

#include <string>
#include <string_view>

namespace alpacacore::util {

/**
 * @brief Build the error message for a failed serial auto-detect.
 *
 * @param device_label Human-readable device name, e.g. "iOptron mount".
 */
inline std::string serial_auto_detect_failed_message(std::string_view device_label) {
    return "No " + std::string(device_label) + " found on any serial port.";
}

}  // namespace alpacacore::util

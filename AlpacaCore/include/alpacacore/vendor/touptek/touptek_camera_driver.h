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

#include <alpacacore/camera_driver.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::touptek {

class ToupTekSDK;  // fault-injection seam (touptek_sdk_wrapper.h, issue #104)

/**
 * @brief Create a ToupTek camera driver by camera index (enumeration order).
 *
 * @param device_number Alpaca device number
 * @param camera_index ToupTek SDK camera index (0-based)
 * @return Unique pointer to camera driver
 */
std::unique_ptr<CameraDriver> create_touptek_camera(int device_number, int camera_index);

// Test seam: identical driver wired to an injected SDK implementation.
std::unique_ptr<CameraDriver> create_touptek_camera(int device_number, int camera_index, ToupTekSDK& sdk);

/**
 * The identity a ToupTek-family camera presents. OEM brands (Altair) run this
 * same driver over their own copy of the SDK and differ only in these strings.
 */
struct ToupCameraBranding {
    std::string label;             // "ToupTek": names, descriptions, log tag, refusal text
    std::string unique_id_prefix;  // "TOUPTEK": <prefix>_SN_<serial>, else <prefix>_<device number>
};

/**
 * @brief Create a ToupTek-family camera driver under the given brand.
 *
 * create_touptek_camera() is this with ToupTek branding; OEM factories
 * (create_altair_camera) call it with their own SDK and branding.
 */
std::unique_ptr<CameraDriver> create_toupcam_family_camera(int device_number, int camera_index, ToupTekSDK& sdk,
                                                           ToupCameraBranding branding);

} // namespace alpacacore::vendor::touptek

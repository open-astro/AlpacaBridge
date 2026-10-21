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

namespace alpacacore::vendor::touptek {
class ToupTekSDK;  // fault-injection seam (touptek_sdk_wrapper.h, issue #104)
}

namespace alpacacore::vendor::altair {

/**
 * @brief Create an Altair Astro camera driver by camera index.
 *
 * Altair cameras are ToupTek OEM hardware on a rebranded ToupTek SDK, so this
 * is the ToupTek camera driver with Altair identity strings, running over
 * AltairSDKWrapper (libaltaircam) instead of libtoupcam.
 *
 * @param device_number Alpaca device number
 * @param camera_index Index into the Altair SDK's camera enumeration (0-based;
 *                     filter wheels and focusers are excluded)
 */
std::unique_ptr<CameraDriver> create_altair_camera(int device_number, int camera_index);

// Test seam: identical driver wired to an injected SDK implementation.
std::unique_ptr<CameraDriver> create_altair_camera(int device_number, int camera_index, touptek::ToupTekSDK& sdk);

}  // namespace alpacacore::vendor::altair

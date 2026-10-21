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

#include <alpacacore/vendor/altair/altair_camera_driver.h>
#include <alpacacore/vendor/altair/altair_sdk_wrapper.h>
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>

namespace alpacacore::vendor::altair {

namespace {

touptek::ToupCameraBranding altair_branding() { return touptek::ToupCameraBranding{"Altair", "ALTAIR"}; }

}  // namespace

std::unique_ptr<CameraDriver> create_altair_camera(int device_number, int camera_index) {
    return create_altair_camera(device_number, camera_index, AltairSDKWrapper::instance());
}

std::unique_ptr<CameraDriver> create_altair_camera(int device_number, int camera_index, touptek::ToupTekSDK& sdk) {
    return touptek::create_toupcam_family_camera(device_number, camera_index, sdk, altair_branding());
}

}  // namespace alpacacore::vendor::altair

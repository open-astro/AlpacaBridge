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

#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>

namespace alpacacore::vendor::altair {

/**
 * The production Altair camera SDK (altaircamsdk 20260531, libaltaircam).
 *
 * Altair Astro cameras are ToupTek OEM hardware and altaircam is the ToupTek
 * SDK under Altair's name, so this is the shared ToupcamFamilySDK
 * (src/vendors/touptek/toupcam_family_sdk.h) over Altaircam_* calls, presented through the
 * same touptek::ToupTekSDK seam the ToupTek drivers and their fakes use.
 * Driver code reaches it only through that interface.
 *
 * It is a separate singleton from ToupTekSDKWrapper::instance(): each library
 * keeps its own enumeration and open handles, and each wrapper its own
 * reference-counted open map.
 */
class AltairSDKWrapper {
public:
    static touptek::ToupTekSDK& instance();

    AltairSDKWrapper() = delete;
};

}  // namespace alpacacore::vendor::altair

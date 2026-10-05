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

#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>
#include <toupcam.h>

#include "toupcam_family_sdk.h"

namespace alpacacore::vendor::touptek {

namespace {

// Traits for ToupcamFamilySDK, generated from the family's X-macro lists so
// every function and constant the shared wrapper calls exists here. libtoupcam
// takes HToupcam directly, so the calls forward unchanged.
struct ToupcamApi {
    static constexpr const char* kPrefix = "Toupcam";
    using DeviceV2 = ToupcamDeviceV2;
    using FrameInfoV4 = ToupcamFrameInfoV4;

#define TOUPCAM_FORWARD(name)                  \
    template <class... Args>                   \
    static decltype(auto) name(Args... args) { \
        return Toupcam_##name(args...);        \
    }
    ALPACACORE_TOUPCAM_FAMILY_FUNCTIONS(TOUPCAM_FORWARD)
#undef TOUPCAM_FORWARD

#define TOUPCAM_CONSTANT(name) static constexpr auto name = TOUPCAM_##name;
    ALPACACORE_TOUPCAM_FAMILY_CONSTANTS(TOUPCAM_CONSTANT)
#undef TOUPCAM_CONSTANT
};

}  // namespace

ToupTekSDK& ToupTekSDKWrapper::instance() {
    static ToupcamFamilySDK<ToupcamApi> sdk;
    return sdk;
}

} // namespace alpacacore::vendor::touptek

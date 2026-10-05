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

#include <alpacacore/vendor/altair/altair_sdk_wrapper.h>
#include <altaircam.h>

#include <type_traits>

#include "../touptek/toupcam_family_sdk.h"

namespace alpacacore::vendor::altair {

namespace {

// The ToupTekSDK seam carries HToupcam; altaircam takes HAltaircam. Both are
// opaque pointers to a one-int struct that the SDK hands out and only ever
// receives back, so the value round-trips unchanged through either type.
HAltaircam to_altair(HToupcam handle) { return reinterpret_cast<HAltaircam>(handle); }
template <class T>
T to_altair(T value) {
    return value;
}

HToupcam from_altair(HAltaircam handle) { return reinterpret_cast<HToupcam>(handle); }
template <class T>
T from_altair(T value) {
    return value;
}

// Traits for touptek::ToupcamFamilySDK, generated from the family's X-macro
// lists so every function and constant the shared wrapper calls exists here.
struct AltaircamApi {
    static constexpr const char* kPrefix = "Altaircam";
    using DeviceV2 = AltaircamDeviceV2;
    using FrameInfoV4 = AltaircamFrameInfoV4;

#define ALTAIR_FORWARD(name)                                           \
    template <class... Args>                                           \
    static decltype(auto) name(Args... args) {                         \
        using Result = decltype(Altaircam_##name(to_altair(args)...)); \
        if constexpr (std::is_void_v<Result>) {                        \
            Altaircam_##name(to_altair(args)...);                      \
        } else {                                                       \
            return from_altair(Altaircam_##name(to_altair(args)...));  \
        }                                                              \
    }
    ALPACACORE_TOUPCAM_FAMILY_FUNCTIONS(ALTAIR_FORWARD)
#undef ALTAIR_FORWARD

#define ALTAIR_CONSTANT(name) static constexpr auto name = ALTAIRCAM_##name;
    ALPACACORE_TOUPCAM_FAMILY_CONSTANTS(ALTAIR_CONSTANT)
#undef ALTAIR_CONSTANT
};

}  // namespace

touptek::ToupTekSDK& AltairSDKWrapper::instance() {
    static touptek::ToupcamFamilySDK<AltaircamApi> sdk;
    return sdk;
}

}  // namespace alpacacore::vendor::altair

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

// The shared ToupTek-family wrapper (ToupcamFamilySDK<Api>) driven through the
// ToupTekSDK interface over a fake Api traits struct (issue #2088). Every other
// ToupTek and Altair test runs over FakeToupTekSDK, above this wrapper. No
// vendor SDK header or library is involved: FakeApi models only the types and
// calls the template uses, once per brand prefix.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "../src/vendors/touptek/toupcam_family_sdk.h"
#include "catch2_compat.h"

using alpacacore::AlpacaException;
using alpacacore::vendor::touptek::ToupcamFamilySDK;
using alpacacore::vendor::touptek::ToupTekSDK;
namespace AlpacaError = alpacacore::AlpacaError;

namespace {

struct ToupcamBrand {
    static constexpr const char* kPrefix = "Toupcam";
};
struct AltairBrand {
    static constexpr const char* kPrefix = "Altaircam";
};

// Distinct ids so each brand's constant list ends up as distinct bit values.
enum FakeConstantIndex {
#define FAKE_CONSTANT_INDEX(name) kIndex_##name,
    ALPACACORE_TOUPCAM_FAMILY_CONSTANTS(FAKE_CONSTANT_INDEX)
#undef FAKE_CONSTANT_INDEX
};

constexpr int kE_INVALIDARG = static_cast<int>(0x80070057u);
constexpr int kE_TIMEOUT = static_cast<int>(0x8001011fu);

template <class Brand>
struct FakeApi {
    static constexpr const char* kPrefix = Brand::kPrefix;

    struct Resolution {
        unsigned width;
        unsigned height;
    };
    struct Model {
        const char* name;
        unsigned long long flag;
        unsigned maxfanspeed;
        unsigned preview;
        float xpixsz;
        float ypixsz;
        Resolution res[1];
    };
    struct DeviceV2 {
        const char* displayname;
        const char* id;
        const Model* model;
    };
    struct FrameInfoV4 {
        struct {
            unsigned width;
            unsigned height;
        } v3;
    };

    // Generated from the same list as the real traits, so no constant is missed;
    // +3 makes MAX 8, enough room for the canned enumeration.
#define FAKE_CONSTANT(name) static constexpr unsigned long long name = 1ULL << (kIndex_##name + 3);
    ALPACACORE_TOUPCAM_FAMILY_CONSTANTS(FAKE_CONSTANT)
#undef FAKE_CONSTANT

    // Calls no case observes answer S_OK.
#define FAKE_NOOP(name)        \
    template <class... Args>   \
    static int name(Args...) { \
        return 0;              \
    }
    FAKE_NOOP(StartPullModeWithCallback)
    FAKE_NOOP(Stop)
    FAKE_NOOP(Trigger)
    FAKE_NOOP(get_ExpTimeRange)
    FAKE_NOOP(put_AutoExpoEnable)
    FAKE_NOOP(get_ExpoAGainRange)
    FAKE_NOOP(get_ExpoAGain)
    FAKE_NOOP(put_ExpoAGain)
    FAKE_NOOP(get_Roi)
    FAKE_NOOP(put_Roi)
    FAKE_NOOP(get_Option)
    FAKE_NOOP(get_Size)
    FAKE_NOOP(get_FinalSize)
    FAKE_NOOP(get_RawFormat)
    FAKE_NOOP(get_Temperature)
    FAKE_NOOP(get_SerialNumber)
    FAKE_NOOP(get_FwVersion)
    FAKE_NOOP(get_PixelSize)
    FAKE_NOOP(ST4PlusGuide)
    FAKE_NOOP(ST4PlusGuideState)
    FAKE_NOOP(AAF)
#undef FAKE_NOOP

    // --- scripted state, reset by every case ---------------------------------
    static inline Model camera_model{"cam", FLAG_MONO | FLAG_ST4, 0, 1, 2.4f, 2.4f, {{3000, 2000}}};
    static inline Model wheel_model{"afw", FLAG_FILTERWHEEL, 0, 0, 0.0f, 0.0f, {{0, 0}}};
    static inline Model focuser_model{"aaf", FLAG_AUTOFOCUSER, 0, 0, 0.0f, 0.0f, {{0, 0}}};
    static inline int handle_storage[4]{};
    static inline bool open_returns_null = false;
    static inline int open_calls = 0;
    static inline int close_calls = 0;
    static inline int put_expo_time_hr = 0;
    static inline int put_option_hr = 0;

    // WaitImageV4 rendezvous: parks until get_ExpoTime runs, bounded so a
    // regression fails the case instead of hanging it.
    static inline std::mutex wait_mutex;
    static inline std::condition_variable wait_cv;
    static inline bool wait_entered = false;
    static inline bool wait_released = false;

    static void reset() {
        open_returns_null = false;
        open_calls = 0;
        close_calls = 0;
        put_expo_time_hr = 0;
        put_option_hr = 0;
        std::lock_guard<std::mutex> lock(wait_mutex);
        wait_entered = false;
        wait_released = false;
    }

    static HToupcam handle(int n) { return reinterpret_cast<HToupcam>(&handle_storage[n]); }

    static const char* Version() { return "fake"; }

    // A camera, an AFW wheel, an AAF focuser, a device with no model block,
    // then a second camera.
    static unsigned EnumV2(DeviceV2* arr) {
        arr[0] = {"Cam A", "id-cam-a", &camera_model};
        arr[1] = {"Wheel", "id-wheel", &wheel_model};
        arr[2] = {"Focuser", "id-focuser", &focuser_model};
        arr[3] = {"Ghost", "id-ghost", nullptr};
        arr[4] = {"Cam B", "id-cam-b", &camera_model};
        return 5;
    }

    static HToupcam Open(const char*) {
        ++open_calls;
        return open_returns_null ? nullptr : handle(0);
    }

    static int Close(HToupcam) {
        ++close_calls;
        return 0;
    }

    static int put_ExpoTime(HToupcam, unsigned) { return put_expo_time_hr; }
    static int put_Option(HToupcam, unsigned long long, int) { return put_option_hr; }

    static int get_ExpoTime(HToupcam, unsigned* value) {
        std::lock_guard<std::mutex> lock(wait_mutex);
        wait_released = true;
        wait_cv.notify_all();
        *value = 1000;
        return 0;
    }

    static int WaitImageV4(HToupcam, unsigned, void*, int, int, int, FrameInfoV4* info) {
        std::unique_lock<std::mutex> lock(wait_mutex);
        wait_entered = true;
        wait_cv.notify_all();
        if (!wait_cv.wait_for(lock, std::chrono::seconds(2), [] { return wait_released; })) {
            return kE_TIMEOUT;
        }
        info->v3.width = 640;
        info->v3.height = 480;
        return 0;
    }
};

template <class Brand>
void check_enumerate_cameras_skips_accessories() {
    using Api = FakeApi<Brand>;
    Api::reset();
    ToupcamFamilySDK<Api> wrapper;
    ToupTekSDK& sdk = wrapper;

    const auto cameras = sdk.enumerate_cameras();

    REQUIRE(cameras.size() == 2);
    CHECK(cameras[0].index == 0);
    CHECK(cameras[0].id == "id-cam-a");
    CHECK(cameras[0].name == "Cam A");
    CHECK(cameras[0].max_width == 3000);
    CHECK_FALSE(cameras[0].is_color);
    CHECK(cameras[0].supports_pulse_guide);
    CHECK(cameras[1].index == 1);
    CHECK(cameras[1].id == "id-cam-b");
    // The accessory enumerations still see their own device.
    CHECK(sdk.enumerate_filter_wheels().size() == 1);
    CHECK(sdk.enumerate_focusers().size() == 1);
}

template <class Brand>
void check_shared_open_ledger() {
    using Api = FakeApi<Brand>;
    Api::reset();
    ToupcamFamilySDK<Api> wrapper;
    ToupTekSDK& sdk = wrapper;

    // A failed open publishes nothing: the next open calls Open again.
    Api::open_returns_null = true;
    try {
        sdk.open_camera_by_id("id-cam-a");
        FAIL("open over a null Open did not throw");
    } catch (const AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::NotConnected);
        CHECK(std::string(e.what()) == std::string(Brand::kPrefix) + "_Open returned null (camera not available)");
    }
    CHECK(Api::open_calls == 1);
    Api::open_returns_null = false;

    // Camera, focuser and filter wheel on one id share one Open.
    const HToupcam h = sdk.open_camera_by_id("id-cam-a");
    CHECK(h == Api::handle(0));
    CHECK(Api::open_calls == 2);
    CHECK(sdk.open_focuser_by_id("id-cam-a") == h);
    CHECK(sdk.open_filter_wheel_by_id("id-cam-a") == h);
    CHECK(Api::open_calls == 2);

    // Close fires only for the last holder, and never for a double close.
    sdk.close_camera(h);
    sdk.close_focuser(h);
    CHECK(Api::close_calls == 0);
    sdk.close_filter_wheel(h);
    CHECK(Api::close_calls == 1);
    sdk.close_camera(h);
    CHECK(Api::close_calls == 1);

    // Released means released: the next open is a fresh Open.
    CHECK(sdk.open_camera_by_id("id-cam-a") == h);
    CHECK(Api::open_calls == 3);
    sdk.close_camera(h);
    CHECK(Api::close_calls == 2);
}

template <class Brand>
void check_error_text_names_the_brand_function() {
    using Api = FakeApi<Brand>;
    Api::reset();
    ToupcamFamilySDK<Api> wrapper;
    ToupTekSDK& sdk = wrapper;
    const std::string prefix = Brand::kPrefix;

    Api::put_expo_time_hr = kE_INVALIDARG;
    try {
        sdk.put_exposure_us(Api::handle(0), 1000);
        FAIL("put_exposure_us over E_INVALIDARG did not throw");
    } catch (const AlpacaException& e) {
        CHECK(std::string(e.what()) == prefix + "_put_ExpoTime: E_INVALIDARG");
        CHECK(e.error_code() == AlpacaError::InvalidValue);
    }

    Api::put_option_hr = kE_INVALIDARG;
    try {
        sdk.put_binning(Api::handle(0), 2);
        FAIL("put_binning over E_INVALIDARG did not throw");
    } catch (const AlpacaException& e) {
        CHECK(std::string(e.what()) == prefix + "_put_Option(BINNING): E_INVALIDARG");
    }
}

template <class Brand>
void check_wait_image_does_not_take_the_wrapper_lock() {
    using Api = FakeApi<Brand>;
    Api::reset();
    ToupcamFamilySDK<Api> wrapper;
    ToupTekSDK& sdk = wrapper;
    const HToupcam h = Api::handle(0);

    bool delivered = false;
    unsigned width = 0;
    unsigned height = 0;
    std::thread waiter([&] { delivered = sdk.wait_image(h, 60000, nullptr, 16, 0, width, height); });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(Api::wait_mutex);
        entered = Api::wait_cv.wait_for(lock, std::chrono::seconds(5), [] { return Api::wait_entered; });
    }
    // A locked call while WaitImageV4 is parked: it can only get in if
    // wait_image left the wrapper mutex free, and it is what releases the wait.
    const unsigned exposure = sdk.get_exposure_us(h);
    waiter.join();

    REQUIRE(entered);
    CHECK(exposure == 1000);
    CHECK(delivered);
    CHECK(width == 640);
    CHECK(height == 480);
}

}  // namespace

TEST_CASE("ToupcamFamilySDK - enumerate_cameras skips filter wheels, focusers and model-less entries",
          "[touptek][altair][toupcam_family][unit]") {
    SECTION("Toupcam") { check_enumerate_cameras_skips_accessories<ToupcamBrand>(); }
    SECTION("Altaircam") { check_enumerate_cameras_skips_accessories<AltairBrand>(); }
}

TEST_CASE("ToupcamFamilySDK - shared open ledger rolls back a failed open and ignores a double close",
          "[touptek][altair][toupcam_family][unit]") {
    SECTION("Toupcam") { check_shared_open_ledger<ToupcamBrand>(); }
    SECTION("Altaircam") { check_shared_open_ledger<AltairBrand>(); }
}

TEST_CASE("ToupcamFamilySDK - SDK errors are named <prefix>_<function>", "[touptek][altair][toupcam_family][unit]") {
    SECTION("Toupcam") { check_error_text_names_the_brand_function<ToupcamBrand>(); }
    SECTION("Altaircam") { check_error_text_names_the_brand_function<AltairBrand>(); }
}

TEST_CASE("ToupcamFamilySDK - wait_image does not hold the wrapper lock across WaitImageV4",
          "[touptek][altair][toupcam_family][unit]") {
    SECTION("Toupcam") { check_wait_image_does_not_take_the_wrapper_lock<ToupcamBrand>(); }
    SECTION("Altaircam") { check_wait_image_does_not_take_the_wrapper_lock<AltairBrand>(); }
}

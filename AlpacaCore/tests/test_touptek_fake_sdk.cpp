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

// Hardware-free driver tests through the fault-injectable ToupTekSDK seam
// (issue #104). Each scenario reproduces a bug class that previously shipped
// to code review on PR #99 because nothing could exercise it without a camera
// attached.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>
#include <alpacacore/vendor/touptek/touptek_filterwheel_driver.h>
#include <alpacacore/vendor/touptek/touptek_focuser_driver.h>
#include <alpacacore/vendor/touptek/touptek_thermal_switch_driver.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "fake_touptek_sdk.h"

using alpacacore::AlpacaException;
using alpacacore::test::FakeToupTekSDK;

namespace {

FakeToupTekSDK make_fake_with_camera() {
    FakeToupTekSDK fake;
    fake.cameras.push_back(FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    return fake;
}

bool eventually(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

}  // namespace

TEST_CASE("ToupTek camera - connect-path failure releases the shared open (r18 #1)",
          "[touptek][camera][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    // Throw from a post-open connect call: the driver must close the
    // ref-counted open on the way out or the next connect gets a stale handle.
    fake.throw_from.insert("put_trigger_mode");

    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    CHECK_THROWS_AS(driver->set_connected(true), AlpacaException);
    CHECK(driver->get_connected() == false);
    // The open must be balanced — a leaked ref here means the NEXT connect
    // returns the same stale handle with open_count 2 and Close never fires.
    CHECK(fake.ref_count("fake-cam-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);

    // Recovery: clear the fault and the same driver connects cleanly.
    fake.throw_from.clear();
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    CHECK(fake.ref_count("fake-cam-0") == 1);
    driver->set_connected(false);
    CHECK(fake.ref_count("fake-cam-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
}

TEST_CASE("ToupTek camera - cameraIndex resolves into the camera-only enumeration",
          "[touptek][camera][unit][fakesdk]") {
    FakeToupTekSDK fake;
    fake.cameras.push_back(FakeToupTekSDK::default_camera("fake-cam-A", "FakeCam A"));
    fake.cameras.push_back(FakeToupTekSDK::default_camera("fake-cam-B", "FakeCam B"));

    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 1, fake);
    driver->set_connected(true);
    // cameraIndex is a position in enumerate_cameras(), then opened BY ID —
    // index 1 must open the second camera, never an accessory or the first.
    CHECK(fake.last_opened_id == "fake-cam-B");
    driver->set_connected(false);
}

TEST_CASE("ToupTek camera + thermal switch - one physical open, last holder closes (r19 #3)",
          "[touptek][switch][unit][fakesdk]") {
    auto fake = make_fake_with_camera();

    auto camera = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    auto thermal = alpacacore::vendor::touptek::create_touptek_thermal_switch(1, 0, fake);

    camera->set_connected(true);
    thermal->set_connected(true);
    // Shared ref-counted open: the second opener must NOT re-open physically.
    CHECK(fake.physical_opens == 1);
    CHECK(fake.ref_count("fake-cam-0") == 2);

    // Disconnecting the camera keeps the switch's handle alive (heater keeps
    // running); only the last holder physically closes.
    camera->set_connected(false);
    CHECK(fake.physical_closes == 0);
    CHECK(fake.ref_count("fake-cam-0") == 1);

    thermal->set_connected(false);
    CHECK(fake.physical_closes == 1);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("ToupTek AFW - connect homes the wheel and waits out a deceleration bounce (r18 #4)",
          "[touptek][filterwheel][unit][fakesdk]") {
    FakeToupTekSDK fake;
    FakeToupTekSDK::ToupFilterWheelInfo wheel;
    wheel.id = "fake-afw-0";
    wheel.name = "FakeAFW";
    wheel.model_name = "AFW-M";
    fake.wheels.push_back(wheel);
    // Homing sequence with a deceleration bounce: a single non-negative read
    // (slot 2) mid-home must NOT count as settled; only the trailing stable
    // run of real-slot reads completes the home.
    fake.wheel_position_script = {-1, -1, 2, -1, 0, 0, 0, 0};

    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_id(0, "fake-afw-0", fake);
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    // Homed: position readable, wheel reports the settled slot.
    CHECK(driver->get_position() == 0);
    CHECK(fake.calls["reset_filter_wheel"] == 1);

    driver->set_connected(false);
    CHECK(fake.ref_count("fake-afw-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("ToupTek thermal switch - failed connect releases the shared open", "[touptek][switch][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    fake.throw_from.insert("get_heat_max");

    auto thermal = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0, fake);
    CHECK_THROWS_AS(thermal->set_connected(true), AlpacaException);
    CHECK(thermal->get_connected() == false);
    CHECK(fake.ref_count("fake-cam-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("ToupTek focuser - connect-path failure releases the shared open", "[touptek][focuser][unit][fakesdk]") {
    FakeToupTekSDK fake;
    FakeToupTekSDK::ToupFocuserInfo focuser;
    focuser.id = "fake-aaf-0";
    focuser.name = "FakeAAF";
    focuser.model_name = "AAF";
    fake.focusers.push_back(focuser);
    // Throw from the connect-time range discovery (MaxStep/backlash probing):
    // the open must be balanced on the way out, same as the camera path.
    fake.throw_from.insert("aaf_range");

    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_id(0, "fake-aaf-0", fake);
    CHECK_THROWS_AS(driver->set_connected(true), AlpacaException);
    CHECK(driver->get_connected() == false);
    CHECK(fake.ref_count("fake-aaf-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);

    // Recovery: clear the fault and the same driver connects cleanly.
    fake.throw_from.clear();
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    CHECK(fake.ref_count("fake-aaf-0") == 1);
    driver->set_connected(false);
    CHECK(fake.ref_count("fake-aaf-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("ToupTek focuser - position round-trips through the AAF Set/Get pair", "[touptek][focuser][unit][fakesdk]") {
    FakeToupTekSDK fake;
    FakeToupTekSDK::ToupFocuserInfo focuser;
    focuser.id = "fake-aaf-0";
    focuser.name = "FakeAAF";
    focuser.model_name = "AAF";
    fake.focusers.push_back(focuser);

    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_id(0, "fake-aaf-0", fake);
    driver->set_connected(true);
    // SetPosition (0x01) and GetPosition (0x02) are distinct action codes for
    // one register — the write must be visible to the read.
    driver->move(1234);
    CHECK(driver->get_position() == 1234);
    driver->set_connected(false);
}

TEST_CASE("ToupTek camera - thermal poller primes the cache at connect and is silent during an exposure",
          "[touptek][camera][unit][fakesdk]") {
    // Regression guard for the ATR585M E_UNEXPECTED frame drops: thermal USB
    // control transfers issued while Toupcam_WaitImageV4 is pending make the
    // SDK abandon the frame. The poller must (1) have read the temperature
    // before the client's first DeviceState poll, (2) issue NO thermal SDK
    // call while exposure_active_, while the getters still answer from the
    // cache, and (3) resume once the exposure ends.
    auto fake = make_fake_with_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // (1) The first poller tick runs as soon as set_connected releases mutex_.
    auto wait_until = [](const std::function<bool()>& pred, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!pred() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return pred();
    };
    REQUIRE(wait_until([&] { return fake.call_count("get_temperature_deciC") >= 1; }, std::chrono::milliseconds(2000)));
    CHECK(fake.call_count("get_tec_voltage_deciV") >= 1);  // default fake camera supports the cooler
    CHECK(fake.call_count("get_tec_voltage_max_deciV") == 1);

    // (2) Hold the frame so the exposure stays in flight across two poll intervals.
    fake.hold_wait_image(true);
    driver->start_exposure(0.05, true);
    REQUIRE(wait_until([&] { return fake.call_count("wait_image") >= 1; }, std::chrono::milliseconds(2000)));
    const int temp_calls = fake.call_count("get_temperature_deciC");
    const int tec_calls = fake.call_count("get_tec_voltage_deciV");
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    // Getters answer from the cache (no SDK traffic) while exposing.
    CHECK_NOTHROW(driver->get_ccd_temperature());
    CHECK_NOTHROW(driver->get_cooler_power());
    CHECK(fake.call_count("get_temperature_deciC") == temp_calls);
    CHECK(fake.call_count("get_tec_voltage_deciV") == tec_calls);
    CHECK(fake.call_count("get_tec_voltage_max_deciV") == 1);

    // (3) End the exposure; the poller resumes.
    fake.release_wait_image();
    REQUIRE(wait_until(
        [&] { return !driver->get_connected() || driver->get_camera_state() == alpacacore::CameraState::Idle; },
        std::chrono::milliseconds(3000)));
    REQUIRE(wait_until([&] { return fake.call_count("get_temperature_deciC") > temp_calls; },
                       std::chrono::milliseconds(3000)));

    driver->set_connected(false);
    CHECK(fake.ref_count("fake-cam-0") == 0);
}

TEST_CASE("ToupTek camera - malformed frame fails ImageReady and a later valid frame recovers",
          "[touptek][camera][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->set_num_x(2);
    driver->set_num_y(2);

    auto wait_until = [](const std::function<bool()>& pred) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return pred();
    };

    fake.deliver_frame = true;
    fake.delivered_dimensions = std::pair<unsigned, unsigned>{0, 2};
    driver->start_exposure(0.01, true);
    REQUIRE(wait_until([&] { return driver->get_camera_state() == alpacacore::CameraState::Idle; }));
    auto error_code_of = [](const std::function<void()>& call) {
        try {
            call();
            return 0;
        } catch (const AlpacaException& e) {
            return e.error_code();
        }
    };
    CHECK(error_code_of([&] { (void)driver->get_image_ready(); }) == alpacacore::AlpacaError::DriverException);
    CHECK(error_code_of([&] { (void)driver->get_image_array(); }) == alpacacore::AlpacaError::DriverException);

    fake.frame_bytes = {1, 0, 2, 0, 3, 0, 4, 0};
    fake.delivered_dimensions = std::pair<unsigned, unsigned>{1, 2};
    driver->start_exposure(0.01, true);
    REQUIRE(wait_until([&] { return driver->get_camera_state() == alpacacore::CameraState::Idle; }));
    CHECK(error_code_of([&] { (void)driver->get_image_ready(); }) == alpacacore::AlpacaError::DriverException);

    fake.delivered_dimensions.reset();
    driver->start_exposure(0.01, true);
    REQUIRE(wait_until([&] {
        try {
            return driver->get_image_ready();
        } catch (const AlpacaException&) {
            return false;
        }
    }));
    const auto image = driver->get_image_array();
    CHECK(image.width == 2);
    CHECK(image.height == 2);
    CHECK(image.rank == 2);
    CHECK(image.data == std::vector<std::int32_t>{1, 2, 3, 4});
    driver->set_connected(false);
}

namespace {

// Little-endian 16-bit frame of `height` rows at `stride` pixels per row,
// every pixel of row r holding value r + 1 (so a replicated row is visible).
std::vector<std::uint8_t> frame_rows_numbered(int stride, int height) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height) * 2);
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < stride; ++col) {
            const auto value = static_cast<std::uint16_t>(row + 1);
            bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
            bytes.push_back(static_cast<std::uint8_t>(value >> 8));
        }
    }
    return bytes;
}

bool image_ready_eventually(alpacacore::CameraDriver& driver) {
    return eventually([&] {
        try {
            return driver.get_image_ready();
        } catch (const AlpacaException&) {
            return false;
        }
    });
}

}  // namespace

// #977: the SDK rounds the binned OUTPUT down to even, so an odd NumX at bin 2
// used to come back one column short and (since #963) fail the exposure. The
// driver now asks for the next even output size and crops, so the client gets
// num real columns.
TEST_CASE("ToupTek camera - odd NumX at bin 2 reads real pixels from an even-grown ROI",
          "[touptek][camera][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->set_bin_x(2);
    driver->set_bin_y(2);
    driver->set_num_x(3);
    driver->set_num_y(2);

    fake.deliver_frame = true;
    // The fake delivers floor_even(8 / 2) = 4 columns per row; the driver lays
    // them out at a 4-pixel stride (a shorter pitch is E_INVALIDARG) and reads
    // the first 3 of each row. 99 marks the cropped 4th column.
    fake.frame_bytes = {1, 0, 2, 0, 3, 0, 99, 0, 4, 0, 5, 0, 6, 0, 99, 0};
    driver->start_exposure(0.01, true);
    REQUIRE(image_ready_eventually(*driver));

    const auto roi = fake.last_roi();
    CHECK(roi.width == 8U);   // (3 + 1) * 2, not 6
    CHECK(roi.height == 4U);  // 2 * 2 is already even output
    const auto image = driver->get_image_array();
    CHECK(image.width == 3);
    CHECK(image.height == 2);
    CHECK(image.data == std::vector<std::int32_t>{1, 2, 3, 4, 5, 6});
    CHECK(driver->get_num_x() == 3);
    driver->set_connected(false);
}

// #977: a full frame whose binned height is odd (1066 / 3 = 355) cannot be
// grown, the 356th binned row is off the sensor, so the SDK delivers 354 rows
// and the driver fills the last requested row from its neighbour instead of
// failing every full-frame exposure at bin 3 (the SC571CC's 4168 / 3 = 1389).
TEST_CASE("ToupTek camera - odd full frame at bin 3 fills the missing sensor edge row from its neighbour",
          "[touptek][camera][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    fake.cameras[0].max_height = 1066;
    fake.cameras[0].supported_bins = {1, 2, 3, 4};
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->set_bin_x(3);
    driver->set_bin_y(3);
    REQUIRE(driver->get_num_x() == 640);
    REQUIRE(driver->get_num_y() == 355);

    fake.deliver_frame = true;
    fake.frame_bytes = frame_rows_numbered(640, 354);
    driver->start_exposure(0.01, true);
    REQUIRE(image_ready_eventually(*driver));

    const auto roi = fake.last_roi();
    CHECK(roi.height == 1066U);  // the whole even sensor; nothing more exists
    const auto image = driver->get_image_array();
    REQUIRE(image.width == 640);
    REQUIRE(image.height == 355);
    REQUIRE(image.data.size() == 640U * 355U);
    CHECK(image.data[0] == 1);
    CHECK(image.data[353U * 640U] == 354);  // last row the sensor delivered
    CHECK(image.data[354U * 640U] == 354);  // filled from its neighbour
    CHECK(image.data[354U * 640U + 639U] == 354);
    driver->set_connected(false);
}

// #977 second half: at bin 1 an odd NumX is even-padded to a one-wider ROI, and
// WaitImageV4 rejects a row pitch narrower than that frame (E_INVALIDARG), so
// this sub-frame failed on every release. The driver now lays the frame out at
// the delivered width and reads the first num_x columns.
TEST_CASE("ToupTek camera - odd NumX at bin 1 lays the even-padded frame out at its own stride",
          "[touptek][camera][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->set_num_x(3);
    driver->set_num_y(2);

    fake.deliver_frame = true;
    // The fake delivers the 4-pixel even-padded span; 99 marks the cropped column.
    fake.frame_bytes = {1, 0, 2, 0, 3, 0, 99, 0, 4, 0, 5, 0, 6, 0, 99, 0};
    driver->start_exposure(0.01, true);
    REQUIRE(image_ready_eventually(*driver));

    CHECK(fake.last_roi().width == 4U);
    const auto image = driver->get_image_array();
    CHECK(image.width == 3);
    CHECK(image.height == 2);
    CHECK(image.data == std::vector<std::int32_t>{1, 2, 3, 4, 5, 6});
    driver->set_connected(false);
}

// #977: the column counterpart of the row fill. A full frame whose binned width
// is odd (1066 / 3 = 355) cannot be grown, so the SDK delivers 354 columns and
// the driver fills the last requested column from its neighbour.
TEST_CASE("ToupTek camera - odd full frame at bin 3 fills the missing sensor edge column from its neighbour",
          "[touptek][camera][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    fake.cameras[0].max_width = 1066;
    fake.cameras[0].supported_bins = {1, 2, 3, 4};
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->set_bin_x(3);
    driver->set_bin_y(3);
    REQUIRE(driver->get_num_x() == 355);
    REQUIRE(driver->get_num_y() == 360);

    fake.deliver_frame = true;
    // 360 rows at the driver's 355-pixel stride, every pixel of column c = c + 1
    // for the 354 delivered columns; column 354 is never written (stays 0).
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(355) * 360 * 2, 0);
    for (int row = 0; row < 360; ++row) {
        for (int col = 0; col < 354; ++col) {
            const auto value = static_cast<std::uint16_t>(col + 1);
            const std::size_t at = (static_cast<std::size_t>(row) * 355 + static_cast<std::size_t>(col)) * 2;
            bytes[at] = static_cast<std::uint8_t>(value & 0xFF);
            bytes[at + 1] = static_cast<std::uint8_t>(value >> 8);
        }
    }
    fake.frame_bytes = bytes;
    driver->start_exposure(0.01, true);
    REQUIRE(image_ready_eventually(*driver));

    CHECK(fake.last_roi().width == 1066U);
    const auto image = driver->get_image_array();
    REQUIRE(image.width == 355);
    REQUIRE(image.height == 360);
    CHECK(image.data[0] == 1);
    CHECK(image.data[353] == 354);                 // last column the sensor delivered
    CHECK(image.data[354] == 354);                 // filled from its neighbour
    CHECK(image.data[359U * 355U + 354U] == 354);  // same on the last row
    driver->set_connected(false);
}

// #913 still holds: a frame below what the ROI math predicts is malformed. Here
// the fake answers 3 columns for a grown 8-pixel span at bin 2 (4 expected);
// build_image_array alone would accept a one-column deficit, so this pins the
// worker's check against the predicted size.
TEST_CASE("ToupTek camera - a frame shorter than the binned ROI contract still fails",
          "[touptek][camera][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->set_bin_x(2);
    driver->set_bin_y(2);
    driver->set_num_x(3);
    driver->set_num_y(2);

    fake.deliver_frame = true;
    fake.frame_bytes = {1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0};
    fake.delivered_dimensions = std::pair<unsigned, unsigned>{3, 2};
    driver->start_exposure(0.01, true);
    REQUIRE(eventually([&] { return driver->get_camera_state() == alpacacore::CameraState::Idle; }));
    auto error_code_of = [](const std::function<void()>& call) {
        try {
            call();
            return 0;
        } catch (const AlpacaException& e) {
            return e.error_code();
        }
    };
    CHECK(error_code_of([&] { (void)driver->get_image_ready(); }) == alpacacore::AlpacaError::DriverException);
    CHECK(error_code_of([&] { (void)driver->get_image_array(); }) == alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("ToupTek camera - no delivered frame fails ImageReady and ImageArray", "[touptek][camera][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->set_num_x(2);
    driver->set_num_y(2);
    driver->start_exposure(0.01, true);

    const auto image_ready_error = [&] {
        try {
            (void)driver->get_image_ready();
        } catch (const AlpacaException& e) {
            return e.error_code();
        }
        return 0;
    };
    REQUIRE(eventually([&] { return image_ready_error() == alpacacore::AlpacaError::DriverException; }));

    const auto image_array_error = [&] {
        try {
            (void)driver->get_image_array();
        } catch (const AlpacaException& e) {
            return e.error_code();
        }
        return 0;
    };
    CHECK(image_array_error() == alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("ToupTek camera - dimensions above INT_MAX fail before conversion", "[touptek][camera][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    fake.deliver_frame = true;
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->set_num_x(2);
    driver->set_num_y(2);

    const auto has_dimension_limit_error = [](auto&& read) {
        try {
            read();
        } catch (const AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException &&
                   std::string(e.what()).find("ToupTek frame dimensions exceed supported limits") != std::string::npos;
        }
        return false;
    };
    const auto over_limit = static_cast<unsigned>(std::numeric_limits<int>::max()) + 1U;
    for (const auto& dimensions : {std::pair<unsigned, unsigned>{over_limit, 2U}, {2U, over_limit}}) {
        fake.delivered_dimensions = dimensions;
        driver->start_exposure(0.01, true);
        REQUIRE(eventually([&] { return has_dimension_limit_error([&] { (void)driver->get_image_ready(); }); }));
        CHECK(has_dimension_limit_error([&] { (void)driver->get_image_array(); }));
    }
    driver->set_connected(false);
}

TEST_CASE("ToupTek camera - a frame arriving after the watchdog fails both image readers",
          "[touptek][camera][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    fake.deliver_frame = true;
    fake.delivered_dimensions = std::pair<unsigned, unsigned>{2U, 2U};

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool wait_image_entered = false;
    bool release_wait_image = false;
    fake.before_call = [&](const std::string& method) {
        if (method != "wait_image") return;
        std::unique_lock<std::mutex> lock(gate_mutex);
        wait_image_entered = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_wait_image; });
    };

    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->set_num_x(2);
    driver->set_num_y(2);
    driver->start_exposure(0.01, true);

    bool waiting = false;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        waiting = gate_cv.wait_for(lock, std::chrono::seconds(2), [&] { return wait_image_entered; });
    }
    CHECK(waiting);
    if (!waiting) {
        {
            std::lock_guard<std::mutex> lock(gate_mutex);
            release_wait_image = true;
        }
        gate_cv.notify_all();
        driver->set_connected(false);
        return;
    }

    const bool watchdog_expired = eventually(
        [&] { return driver->get_camera_state() == alpacacore::CameraState::Idle; }, std::chrono::seconds(17));
    CHECK(watchdog_expired);
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_wait_image = true;
    }
    gate_cv.notify_all();
    if (!watchdog_expired) {
        driver->set_connected(false);
        return;
    }

    const auto is_late_frame_failure = [](auto&& read) {
        try {
            read();
        } catch (const AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException &&
                   std::string(e.what()).find("frame arrived after the exposure was no longer active") !=
                       std::string::npos;
        }
        return false;
    };
    REQUIRE(eventually([&] { return is_late_frame_failure([&] { (void)driver->get_image_ready(); }); }));
    CHECK(is_late_frame_failure([&] { (void)driver->get_image_array(); }));
    driver->set_connected(false);
}

TEST_CASE("ToupTek camera - aborting a held image wait leaves ImageReady false without an error",
          "[touptek][camera][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    fake.hold_wait_image(true);
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->start_exposure(0.01, true);
    REQUIRE(eventually([&] { return fake.call_count("wait_image") > 0; }));

    driver->abort_exposure();
    CHECK_FALSE(driver->get_image_ready());
    driver->set_connected(false);
}

TEST_CASE("ToupTek camera - malformed exposure failure clears on reconnect", "[touptek][camera][unit][fakesdk]") {
    auto fake = make_fake_with_camera();
    fake.deliver_frame = true;
    fake.delivered_dimensions = std::pair<unsigned, unsigned>{0, 2};
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->set_num_x(2);
    driver->set_num_y(2);
    driver->start_exposure(0.01, true);
    REQUIRE(eventually([&] {
        try {
            (void)driver->get_image_ready();
        } catch (const AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    }));

    driver->set_connected(false);
    driver->set_connected(true);
    CHECK_FALSE(driver->get_image_ready());
    driver->set_connected(false);
}

// open-astro#294: a wheel's DeviceState is the single Position member, so it already costs one
// device read and the static members cost none; this pins that so a cache is not needed.
// Falsified by: touptek_filterwheel_driver.cpp get_names() calling
// sdk_.get_filter_wheel_position() (position reads 4 after the loop, not 1).
TEST_CASE("ToupTek AFW - DeviceState is one device read and static members cost none",
          "[touptek][filterwheel][unit][fakesdk]") {
    FakeToupTekSDK fake;
    FakeToupTekSDK::ToupFilterWheelInfo wheel;
    wheel.id = "fake-afw-0";
    wheel.name = "FakeAFW";
    wheel.model_name = "AFW-M";
    fake.wheels.push_back(wheel);
    fake.wheel_position_script = {0, 0, 0, 0, 0, 0, 0, 0};

    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_id(0, "fake-afw-0", fake);
    driver->set_connected(true);
    const int base = fake.call_count("get_filter_wheel_position");

    CHECK(driver->get_device_state().size() == 2);  // Position, TimeStamp
    CHECK(fake.call_count("get_filter_wheel_position") - base == 1);
    for (int i = 0; i < 3; ++i) {
        (void)driver->get_names();
        (void)driver->get_focus_offsets();
    }
    CHECK(fake.call_count("get_filter_wheel_position") - base == 1);
    driver->set_connected(false);
}

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

// Hardware-free driver tests through the fault-injectable GPhotoSDK/RawDecoder
// seams (issue #489). Before this, the gphoto camera driver's connect,
// widget-fallback, ISO, and bulb-capture code paths were only exercisable
// against real DSLR hardware.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/gphoto/gphoto_camera_driver.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "camera_percent_cooler_checks.h"
#include "catch2_compat.h"
#include "fake_gphoto_sdk.h"
#include "fake_raw_decoder.h"
#include "fake_task_clock.h"

using alpacacore::AlpacaException;
using alpacacore::test::FakeGPhotoSDK;
using alpacacore::test::FakeRawDecoder;
using alpacacore::test::reset_gphoto_sensor_cache;
using alpacacore::test::unique_test_model;

namespace {

FakeGPhotoSDK::FakeCamera make_camera(const std::string& model = "Nikon DSC D5300",
                                      const std::string& port = "usb:001,004") {
    FakeGPhotoSDK::FakeCamera cam;
    cam.model = unique_test_model(model);
    cam.port = port;
    cam.choices["iso"] = {"100", "200", "400", "800"};
    cam.choice_value["iso"] = "200";
    cam.choices["shutterspeed2"] = {"1/4000", "1/200", "1", "30", "bulb"};
    cam.choice_value["shutterspeed2"] = "1/200";
    cam.toggle_value["bulb"] = false;
    cam.choices["imageformat"] = {"NEF (Raw)", "JPEG Fine"};
    cam.choice_value["imageformat"] = "JPEG Fine";
    return cam;
}

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

// A camera that left the USB bus mid-exposure disconnects itself, and CameraState then throws NotConnected:
// no exposure is in progress either way.
void wait_for_exposure_to_finish(alpacacore::CameraDriver& driver) {
    for (int i = 0; i < 500; ++i) {
        try {
            if (driver.get_camera_state() != alpacacore::CameraState::Exposing) return;
        } catch (const AlpacaException& e) {
            if (e.error_code() == alpacacore::AlpacaError::NotConnected) return;
            throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// A camera whose longest native shutter choice is very short (1/50s), so a
// bulb test can request a still-fast exposure (well under a second of real
// sleeping in bulb_capture_with_abort's slice loop) that nonetheless exceeds
// every native choice and forces the driver into the bulb toggle sequence.
FakeGPhotoSDK::FakeCamera make_bulb_camera() {
    auto cam = make_camera("Nikon DSC D5300 (bulb)", "usb:001,005");
    cam.choices["shutterspeed2"] = {"1/4000", "1/200", "1/50", "bulb"};
    cam.choice_value["shutterspeed2"] = "1/200";
    return cam;
}

// Canon EOS body: no standalone "bulb" widget, bulb is the shutterspeed choice plus the eosremoterelease
// press/release pair (issue #640).
FakeGPhotoSDK::FakeCamera make_canon_camera() {
    auto cam = make_camera("Canon EOS 250D", "usb:001,006");
    cam.toggle_value.erase("bulb");
    cam.choices["shutterspeed2"] = {"1/4000", "1/200", "1/50", "bulb"};
    cam.choice_value["shutterspeed2"] = "1/200";
    cam.choices["eosremoterelease"] = {"None", "Press Half", "Press Full", "Release Half", "Release Full"};
    cam.choice_value["eosremoterelease"] = "None";
    return cam;
}

// A failed exposure is reported by ImageReady and ImageArray, not by a sticky CameraState Error (issue #1204,
// the ZWO shape): both throw DriverException naming the SDK failure.
void require_exposure_failure_reported(const alpacacore::CameraDriver& driver, const std::string& sdk_text) {
    const std::function<void()> calls[] = {[&] { (void)driver.get_image_ready(); },
                                           [&] { (void)driver.get_image_array(); }};
    for (const auto& call : calls) {
        try {
            call();
            FAIL("ImageReady and ImageArray must throw after a failed exposure");
        } catch (const AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(e.what()).find("Exposure failed: ") != std::string::npos);
            CHECK(std::string(e.what()).find(sdk_text) != std::string::npos);
        }
    }
}

}  // namespace

TEST_CASE("GPhoto camera fake - connect populates handle and balances open/close", "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    CHECK(fake.open_count == 1);
    CHECK(fake.close_count == 0);
    // Connect must have taken the priming capture+decode, not the on-disk
    // sensor-cache shortcut -- otherwise reset_gphoto_sensor_cache() above
    // has silently stopped working and every test here loses that coverage.
    CHECK(decoder.decode_call_count.load() >= 1);
    // Priming must not look like a real exposure to an ASCOM client.
    CHECK(driver->get_image_ready() == false);

    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
    CHECK(fake.close_count == 1);
}

TEST_CASE("GPhoto camera fake - connected PulseGuide is NotImplemented and names no library",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    REQUIRE(driver->get_connected() == true);

    // The message reaches the ASCOM client, so it names the kind of camera,
    // not the library the driver happens to be built on.
    bool threw = false;
    try {
        driver->pulse_guide(0, 100);
    } catch (const AlpacaException& e) {
        threw = true;
        CHECK(e.error_code() == alpacacore::AlpacaError::NotImplemented);
        const std::string msg = e.what();
        std::string lowered = msg;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        CHECK(lowered.find("gphoto") == std::string::npos);
        CHECK(msg.find("DSLR / mirrorless camera") != std::string::npos);
    }
    CHECK(threw);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - connected cooler members raise PropertyNotImplemented",
          "[gphoto][camera][unit][fakesdk]") {
    // While disconnected these throw NotConnected first (driver-lifecycle.instructions.md "ASCOM contract precedence",
    // #658); once connected a camera without a cooler raises PropertyNotImplemented (ICameraV4 CoolerOn,
    // CoolerPower: "if not supported"), never a made-up false or 0.0 (issue #1477).
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    REQUIRE(driver->get_connected() == true);

    CHECK_FALSE(driver->get_can_get_cooler_power());
    alpacacore::test::require_no_cooler_members(*driver);
    for (auto fn : {std::function<void()>([&] { (void)driver->get_set_ccd_temperature(); }),
                    std::function<void()>([&] { driver->set_set_ccd_temperature(0.0); })}) {
        bool threw = false;
        try {
            fn();
        } catch (const AlpacaException& e) {
            threw = true;
            CHECK(e.error_code() == alpacacore::AlpacaError::NotImplemented);
        }
        CHECK(threw);
    }

    driver->set_connected(false);
}

// Issue #1477: ICameraV4 PercentCompleted raises InvalidOperation outside Exposing.
TEST_CASE("GPhoto camera fake - PercentCompleted only while Exposing", "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_bulb_camera());
    FakeRawDecoder decoder;
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    SECTION("idle before the first exposure") { alpacacore::test::require_percent_completed_unavailable(*driver); }
    SECTION("idle after a completed exposure") {
        driver->start_exposure(0.01, true);
        wait_for_exposure_to_finish(*driver);
        REQUIRE(driver->get_image_ready());
        alpacacore::test::require_percent_completed_unavailable(*driver);
    }
    SECTION("idle after a failed exposure") {
        fake.throw_from.insert("drain_events");
        driver->start_exposure(0.25, true);  // > 1/50 s native ceiling => bulb path
        wait_for_exposure_to_finish(*driver);
        require_exposure_failure_reported(*driver, "injected failure in drain_events");
        alpacacore::test::require_percent_completed_unavailable(*driver);
    }
    SECTION("while exposing") {
        driver->start_exposure(5.0, true);
        alpacacore::test::require_percent_completed_available(*driver);
        driver->stop_exposure();
    }
    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - connect with no camera at the index names no library",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;  // no cameras attached
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    bool threw = false;
    try {
        driver->set_connected(true);
    } catch (const AlpacaException& e) {
        threw = true;
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
        const std::string msg = e.what();
        std::string lowered = msg;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        CHECK(lowered.find("gphoto") == std::string::npos);
        CHECK(msg.find("plugged in") != std::string::npos);
    }
    CHECK(threw);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("GPhoto camera fake - connect-path throw closes the handle and leaves disconnected",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    fake.throw_from.insert("get_choices");
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    CHECK_THROWS_AS(driver->set_connected(true), AlpacaException);
    CHECK(driver->get_connected() == false);
    // The connect-path open must be balanced by a close on the failure path.
    CHECK(fake.open_count == fake.close_count);

    fake.throw_from.clear();
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
}

TEST_CASE("GPhoto camera fake - shutter widget fallback to shutterspeed when shutterspeed2 absent",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_camera();
    cam.choices.erase("shutterspeed2");
    cam.choice_value.erase("shutterspeed2");
    // 1/2000 deliberately differs from min_native_shutter_seconds_'s 0.001
    // default: asserting on a value the default already satisfies would pass
    // even with the whole fallback loop removed. See the negative control below.
    cam.choices["shutterspeed"] = {"1/2000", "1", "bulb"};
    cam.choice_value["shutterspeed"] = "1/2000";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    // Only reachable if the probe fell through shutterspeed2 to shutterspeed
    // AND read its choices: the default would leave this at 0.001.
    CHECK(driver->get_exposure_min() < 0.0009);
    CHECK(driver->get_exposure_min() > 0.0001);
    // "bulb" among the fallback widget's choices must still be recognised.
    CHECK(driver->get_exposure_max() == 3600.0);
    driver->set_connected(false);
}

// Negative control for the case above: with no shutter widget under any of the
// three probed names, the driver keeps its defaults. This is what makes the
// 0.0005 assertion above meaningful rather than vacuous -- if the fallback loop
// stopped working, that test would land on exactly these values.
TEST_CASE("GPhoto camera fake - no shutter widget at all leaves the shutter defaults untouched",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_camera();
    for (const char* name : {"shutterspeed2", "shutterspeed", "eos-shutterspeed"}) {
        cam.choices.erase(name);
        cam.choice_value.erase(name);
    }
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    CHECK(driver->get_exposure_min() == 0.001);
    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - ISO get/set round-trip via the iso widget", "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    // Fake camera starts at iso choice_value "200", index 1 in {100,200,400,800}.
    CHECK(driver->get_gain() == 1);

    driver->set_gain(3);
    CHECK(driver->get_gain() == 3);
    CHECK(fake.cameras[0].choice_value.at("iso") == "800");

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - short native exposure decodes into image_ready with fake frame dims",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    CHECK(driver->get_image_ready() == false);

    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_image_ready() == true);
    CHECK(driver->get_camera_x_size() == decoder.canned_frame.width);
    CHECK(driver->get_camera_y_size() == decoder.canned_frame.height);
    CHECK(driver->get_bayer_offset_x() == decoder.canned_frame.bayer_offset_x);
    CHECK(driver->get_bayer_offset_y() == decoder.canned_frame.bayer_offset_y);

    auto image = driver->get_image_array();
    CHECK(image.width == decoder.canned_frame.width);
    CHECK(image.height == decoder.canned_frame.height);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - bulb capture drives toggle true then false then downloads",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_bulb_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    // Longer than the longest native shutter choice (1/50s, from
    // make_bulb_camera()'s shutterspeed2 choices) so the driver must fall
    // back to the bulb toggle sequence, while staying fast to run.
    driver->start_exposure(0.05, true);
    wait_for_exposure_to_finish(*driver);

    REQUIRE(fake.bulb_toggle_history.size() >= 2);
    CHECK(fake.bulb_toggle_history.front() == true);
    CHECK(fake.bulb_toggle_history.back() == false);
    CHECK(driver->get_image_ready() == true);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - stop_exposure mid-bulb closes the shutter early", "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_bulb_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    // Well past the bulb camera's 1/50s native ceiling, so the driver enters
    // the bulb toggle sequence; stop_exposure() aborts it well before the
    // full requested duration elapses.
    driver->start_exposure(5.0, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // stop_exposure() joins the worker, so the toggle history alone cannot
    // tell an early abort from the full 5 s bulb run: pin the "early" half
    // by the clock. The abort poll slices are well under a second; the 2 s
    // bound leaves headroom for a loaded runner while staying far from 5 s
    // (review of #546).
    const auto stop_started = std::chrono::steady_clock::now();
    driver->stop_exposure();
    const auto stop_took = std::chrono::steady_clock::now() - stop_started;
    CHECK(stop_took < std::chrono::seconds(2));

    REQUIRE(fake.bulb_toggle_history.size() >= 2);
    CHECK(fake.bulb_toggle_history.front() == true);
    CHECK(fake.bulb_toggle_history.back() == false);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - bulb hold pumps camera events for the whole exposure instead of sleeping",
          "[gphoto][camera][unit][fakesdk]") {
    // Issue #569: a Nikon D3300 wedged its PTP stack when the shutter-close
    // toggle arrived after a hold spent in a plain sleep, while the gphoto2
    // CLI's --wait-event hold (polling events throughout) closed cleanly
    // every time. Pin that the hold is spent in drain_events() and that the
    // slices add up to the requested duration, so a future "simplification"
    // back to sleep_for shows up here rather than on the bench.
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_bulb_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    constexpr double kDuration = 0.25;  // > 1/50 s native ceiling => bulb path
    driver->start_exposure(kDuration, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_image_ready() == true);
    // A regression back to sleep_for pumps zero slices, so one slice is the
    // discriminating bound. Each slice's budget is min(remaining, 100 ms),
    // so an oversleep on a loaded sanitizer runner shrinks the sum rather
    // than growing it; a tighter floor here was a flake, not a check.
    CHECK(fake.drain_events_calls >= 1);
    CHECK(fake.drained_budget >= std::chrono::milliseconds(100));
    CHECK(fake.drained_budget <= std::chrono::milliseconds(400));
    REQUIRE(fake.bulb_toggle_history.size() == 2);
    CHECK(fake.bulb_toggle_history.front() == true);
    CHECK(fake.bulb_toggle_history.back() == false);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - bulb close is sent even when the hold loop throws", "[gphoto][camera][unit][fakesdk]") {
    // The hold between bulb=1 and bulb=0 now calls into the SDK (drain_events)
    // rather than sleeping, so it can throw. A throw that skips the close
    // leaves the shutter open, the exact wedge issue #569 is about, so the
    // close is guarded: it must still be sent, and the exposure then fails
    // normally (state back to Idle, ImageReady and ImageArray name the failure).
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_bulb_camera());
    fake.throw_from.insert("drain_events");
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    driver->start_exposure(0.25, true);  // > 1/50 s native ceiling => bulb path
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    require_exposure_failure_reported(*driver, "injected failure in drain_events");
    REQUIRE(fake.bulb_toggle_history.size() == 2);
    CHECK(fake.bulb_toggle_history.front() == true);
    CHECK(fake.bulb_toggle_history.back() == false);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - bulb frame poll keeps polling until the camera delivers the file",
          "[gphoto][camera][unit][fakesdk]") {
    // Issue #569: the file-added event can lag the shutter close by up to a
    // second exposure-length (long-exposure noise reduction), so a fixed
    // 15 s wait lost long frames. The driver must poll in bounded slices
    // (so stop/abort stay responsive) and keep going past a few empty ones.
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_bulb_camera());
    fake.bulb_file_polls_before_ready = 3;
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    driver->start_exposure(0.05, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_image_ready() == true);
    CHECK(fake.bulb_file_polls == 4);  // three "not yet" answers, then the frame
    REQUIRE(!fake.bulb_file_poll_timeouts.empty());
    for (const auto& timeout : fake.bulb_file_poll_timeouts) {
        CHECK(timeout > std::chrono::milliseconds(0));
        CHECK(timeout <= std::chrono::seconds(1));
    }

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - decoder failure leaves camera idle and ImageReady reports the failure",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;
    decoder.should_throw = true;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    require_exposure_failure_reported(*driver, "injected decode failure");

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - Canon bulb presses then releases eosremoterelease by name",
          "[gphoto][camera][unit][fakesdk][canon]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_canon_camera();
    // Reordered so an index-based pick would choose the wrong entries.
    cam.choices["eosremoterelease"] = {"Release Full", "Press Full", "None"};
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    CHECK(driver->get_exposure_max() == 3600.0);

    driver->start_exposure(0.05, true);  // > 1/50 s native ceiling
    wait_for_exposure_to_finish(*driver);

    REQUIRE(fake.remote_release_history.size() == 2);
    CHECK(fake.remote_release_history.front() == "Press Full");
    CHECK(fake.remote_release_history.back() == "Release Full");
    CHECK(fake.cameras[0].choice_value["shutterspeed2"] == "bulb");
    CHECK(fake.bulb_toggle_history.empty());
    CHECK(driver->get_image_ready() == true);

    driver->set_connected(false);
}

// libgphoto2 2.5.34 renamed the choices (NEWS: "canon eos: remoterelease config values changed").
// "Press Full MF" / "Release" send the same PTP calls as 2.5.31's "Press Full" / "Release Full"
// (remotereleaseon(3, 1) / remotereleaseoff(3)); 2.5.34's "Release Full" is remotereleaseoff(2),
// which leaves the half press held, so it must not be the close.
TEST_CASE("GPhoto camera fake - Canon bulb uses the libgphoto2 2.5.34 eosremoterelease names",
          "[gphoto][camera][unit][fakesdk][canon]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_canon_camera();
    cam.choices["eosremoterelease"] = {"None",          "Press Half AF", "Press Full AF", "Press Half MF",
                                       "Press Full MF", "Release Half",  "Release Full",  "Release"};
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    CHECK(driver->get_exposure_max() == 3600.0);

    driver->start_exposure(0.05, true);  // > 1/50 s native ceiling
    wait_for_exposure_to_finish(*driver);

    REQUIRE(fake.remote_release_history.size() == 2);
    CHECK(fake.remote_release_history.front() == "Press Full MF");
    CHECK(fake.remote_release_history.back() == "Release");
    CHECK(driver->get_image_ready() == true);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - Canon bulb stop_exposure releases early", "[gphoto][camera][unit][fakesdk][canon]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_canon_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    driver->start_exposure(5.0, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto stop_started = std::chrono::steady_clock::now();
    driver->stop_exposure();
    CHECK(std::chrono::steady_clock::now() - stop_started < std::chrono::seconds(2));

    REQUIRE(fake.remote_release_history.size() == 2);
    CHECK(fake.remote_release_history.front() == "Press Full");
    CHECK(fake.remote_release_history.back() == "Release Full");

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - Canon release is sent even when the hold loop throws",
          "[gphoto][camera][unit][fakesdk][canon]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_canon_camera());
    fake.throw_from.insert("drain_events");
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    driver->start_exposure(0.25, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    require_exposure_failure_reported(*driver, "injected failure in drain_events");
    REQUIRE(fake.remote_release_history.size() == 2);
    CHECK(fake.remote_release_history.front() == "Press Full");
    CHECK(fake.remote_release_history.back() == "Release Full");

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - ExposureMax stays native unless both eosremoterelease choices exist",
          "[gphoto][camera][unit][fakesdk][canon]") {
    for (const char* missing : {"Press Full", "Release Full"}) {
        reset_gphoto_sensor_cache();
        FakeGPhotoSDK fake;
        auto cam = make_canon_camera();
        auto& list = cam.choices["eosremoterelease"];
        list.erase(std::remove(list.begin(), list.end(), std::string(missing)), list.end());
        fake.cameras.push_back(cam);
        FakeRawDecoder decoder;

        auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
        driver->set_connected(true);
        CHECK(driver->get_exposure_max() == 1.0 / 50.0);
        driver->set_connected(false);
    }
    // The 2.5.34 pair is matched as a pair too: its "Release Full" alone does not close the press.
    for (const char* missing : {"Press Full MF", "Release"}) {
        reset_gphoto_sensor_cache();
        FakeGPhotoSDK fake;
        auto cam = make_canon_camera();
        cam.choices["eosremoterelease"] = {"None", "Press Full AF", "Press Full MF", "Release Full", "Release"};
        auto& list = cam.choices["eosremoterelease"];
        list.erase(std::remove(list.begin(), list.end(), std::string(missing)), list.end());
        fake.cameras.push_back(cam);
        FakeRawDecoder decoder;

        auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
        driver->set_connected(true);
        CHECK(driver->get_exposure_max() == 1.0 / 50.0);
        driver->set_connected(false);
    }
}

TEST_CASE("GPhoto camera fake - Canon B mode (only bulb in the shutter list) uses bulb for every duration",
          "[gphoto][camera][unit][fakesdk][canon]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_canon_camera();
    cam.choices["shutterspeed2"] = {"bulb"};
    cam.choice_value["shutterspeed2"] = "bulb";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    CHECK(driver->get_exposure_max() == 3600.0);

    driver->start_exposure(0.01, true);  // shorter than any "native" speed would be
    wait_for_exposure_to_finish(*driver);

    REQUIRE(fake.remote_release_history.size() == 2);
    CHECK(fake.remote_release_history.front() == "Press Full");
    CHECK(driver->get_image_ready() == true);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - Canon B mode does not write the shutter widget",
          "[gphoto][camera][unit][fakesdk][canon]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_canon_camera();
    cam.choices["shutterspeed2"] = {"bulb"};
    cam.choice_value["shutterspeed2"] = "bulb";
    fake.cameras.push_back(cam);
    fake.refuse_choice_writes = {"shutterspeed2"};
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    driver->start_exposure(5.0, true);
    wait_for_exposure_to_finish(*driver);

    REQUIRE(fake.remote_release_history.size() == 2);
    CHECK(fake.remote_release_history.front() == "Press Full");
    CHECK(fake.remote_release_history.back() == "Release Full");
    CHECK(driver->get_image_ready() == true);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - bulb widget camera ignores eosremoterelease", "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_bulb_camera();
    cam.choices["eosremoterelease"] = {"Press Full", "Release Full"};
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    driver->start_exposure(0.05, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(fake.remote_release_history.empty());
    REQUIRE(fake.bulb_toggle_history.size() == 2);
    CHECK(fake.bulb_toggle_history.front() == true);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - Canon bulb entry wins over other unparseable shutter entries",
          "[gphoto][camera][unit][fakesdk][canon]") {
    const std::vector<std::vector<std::string>> lists = {{"bulb", "1/50", "auto"}, {"auto", "1/50", "bulb"}};
    for (const auto& list : lists) {
        reset_gphoto_sensor_cache();
        FakeGPhotoSDK fake;
        auto cam = make_canon_camera();
        cam.choices["shutterspeed2"] = list;
        cam.choice_value["shutterspeed2"] = "1/50";
        fake.cameras.push_back(cam);
        FakeRawDecoder decoder;

        auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
        driver->set_connected(true);
        CHECK(driver->get_exposure_max() == 3600.0);

        driver->start_exposure(0.05, true);
        wait_for_exposure_to_finish(*driver);

        CHECK(fake.cameras[0].choice_value["shutterspeed2"] == "bulb");
        CHECK(fake.remote_release_history.size() == 2);

        driver->set_connected(false);
    }
}

TEST_CASE("GPhoto camera fake - bulb widget camera keeps the first of several unparseable shutter entries",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_bulb_camera();
    cam.choices["shutterspeed2"] = {"1/200", "1/50", "auto", "Unknown value"};
    cam.choice_value["shutterspeed2"] = "1/200";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    driver->start_exposure(0.5, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(fake.cameras[0].choice_value["shutterspeed2"] == "auto");
    CHECK(fake.bulb_toggle_history.size() == 2);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - Canon shutter list with only auto does not enable bulb",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_canon_camera();
    cam.choices["shutterspeed2"] = {"auto"};
    cam.choice_value["shutterspeed2"] = "auto";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    CHECK(driver->get_exposure_max() != 3600.0);

    try {
        driver->start_exposure(1.0, true);
        FAIL("start_exposure should refuse a body with no bulb entry");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::NotImplemented);
    }
    CHECK(fake.remote_release_history.empty());

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - camera gone from the USB bus mid-exposure reports a disconnect",
          "[gphoto][camera][unit][fakesdk][canon]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_canon_camera();
    cam.choices["shutterspeed2"] = {"bulb"};  // B mode, as on the rig: the press is the first call
    cam.choice_value["shutterspeed2"] = "bulb";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    REQUIRE(driver->get_connected() == true);

    std::atomic<bool> armed{true};
    fake.before_call = [&](const std::string& name) {
        if (armed.load() && name == "set_choice_value") {
            throw alpacacore::vendor::gphoto::GPhotoDeviceGoneException("Failed to set eosremoterelease = Press Full",
                                                                        alpacacore::AlpacaError::DriverException);
        }
    };
    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);

    // The body left the bus: the client must see Connected false and reconnect.
    CHECK(driver->get_connected() == false);
    require_alpaca_error([&] { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);

    driver->set_connected(false);  // joins the worker
    CHECK(fake.close_count == fake.open_count);

    // Reconnect enumerates again and the next exposure works.
    armed.store(false);
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);
    CHECK(driver->get_image_ready() == true);

    driver->set_connected(false);
    CHECK(fake.close_count == fake.open_count);
}

TEST_CASE("GPhoto camera fake - failed capture reports the SDK error through ImageReady and stays idle",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    fake.throw_from.insert("capture_and_download");
    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_connected() == true);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    require_exposure_failure_reported(*driver, "injected failure in capture_and_download");

    // The next StartExposure starts and clears the failure.
    fake.throw_from.clear();
    REQUIRE_NOTHROW(driver->start_exposure(0.01, true));
    wait_for_exposure_to_finish(*driver);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_image_ready() == true);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - failed bulb press reports the SDK error through ImageReady and stays idle",
          "[gphoto][camera][unit][fakesdk][canon]") {
    // Issue #1204: a Canon in B mode whose eosremoterelease press fails. The failure was in the log only and
    // ImageReady stayed false; it must reach the client without leaving the camera in a sticky Error state.
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_canon_camera();
    cam.choices["shutterspeed2"] = {"bulb"};  // B mode: the press is the first call of the exposure
    cam.choice_value["shutterspeed2"] = "bulb";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    REQUIRE(driver->get_connected() == true);

    std::atomic<bool> armed{true};
    fake.before_call = [&](const std::string& name) {
        if (armed.load() && name == "set_choice_value") {
            throw AlpacaException("Failed to set eosremoterelease = Press Full: Unspecified error",
                                  alpacacore::AlpacaError::DriverException);
        }
    };
    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_connected() == true);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    require_exposure_failure_reported(*driver, "Failed to set eosremoterelease = Press Full: Unspecified error");

    armed.store(false);
    REQUIRE_NOTHROW(driver->start_exposure(0.01, true));
    wait_for_exposure_to_finish(*driver);
    CHECK(driver->get_image_ready() == true);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - abort after a failed exposure clears the reported failure",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    fake.throw_from.insert("capture_and_download");
    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);
    require_exposure_failure_reported(*driver, "injected failure in capture_and_download");

    driver->abort_exposure();
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_image_ready() == false);
    try {
        (void)driver->get_image_array();
        FAIL("ImageArray must throw with no image");
    } catch (const AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::InvalidOperation);
        CHECK(std::string(e.what()).find("Exposure failed") == std::string::npos);
    }

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - camera gone while an abort closes the shutter keeps the camera connected",
          "[gphoto][camera][unit][fakesdk][canon]") {
    // A stop in progress is not a failure: a device-gone error raised after AbortExposure (here by the
    // release that closes the shutter) must not close the handle or report a disconnect.
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_canon_camera();
    cam.choices["shutterspeed2"] = {"bulb"};  // B mode: press, hold, release
    cam.choice_value["shutterspeed2"] = "bulb";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    REQUIRE(driver->get_connected() == true);

    std::atomic<bool> holding{false};
    std::atomic<int> choice_writes{0};
    fake.before_call = [&](const std::string& name) {
        if (name == "drain_events") holding.store(true);
        if (name == "set_choice_value" && ++choice_writes == 2) {  // 1: press, 2: release
            throw alpacacore::vendor::gphoto::GPhotoDeviceGoneException("Failed to set eosremoterelease = Release Full",
                                                                        alpacacore::AlpacaError::DriverException);
        }
    };
    driver->start_exposure(10.0, true);  // only the abort ends the hold
    for (int i = 0; i < 500 && !holding.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(holding.load());
    const int closes = fake.close_count;

    driver->abort_exposure();  // joins the worker

    CHECK(choice_writes.load() == 2);
    CHECK(driver->get_connected() == true);
    CHECK(fake.close_count == closes);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_image_ready() == false);

    driver->set_connected(false);
    CHECK(fake.close_count == fake.open_count);
}

// The TaskClock cases below pin each driver wait to the injected clock. Virtual time starts at the epoch while
// the real steady_clock reads machine uptime, so a deadline SET from the real clock never trips against fake
// time, and a CHECK against the real clock trips at once against a fake-time deadline: each case asserts the
// state just before the deadline and just after it, so either mutation turns it red.

TEST_CASE("GPhoto camera fake - exposure watchdog deadline reads the TaskClock",
          "[gphoto][camera][unit][fakesdk][taskclock]") {
    reset_gphoto_sensor_cache();
    alpacacore::test::FakeTaskClock clock;
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;

    // Hold the capture so the exposure stays active; released before the driver goes away.
    std::atomic<bool> armed{false};  // connect makes its own priming capture; hold only the exposure's
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    fake.before_call = [&](const std::string& name) {
        if (name != "capture_and_download" || !armed.load()) return;
        entered.store(true);
        for (int i = 0; i < 5000 && !release.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder, clock);
    driver->set_connected(true);
    armed.store(true);
    driver->start_exposure(1.0, true);  // a native shutter choice, so no bulb loop
    for (int i = 0; i < 2000 && !entered.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(entered.load());

    // The deadline is the exposure plus a 60 s margin on the clock.
    clock.advance(std::chrono::seconds(60));
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    clock.advance(std::chrono::seconds(2));
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);

    release.store(true);
    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - bulb hold deadline reads the TaskClock", "[gphoto][camera][unit][fakesdk][taskclock]") {
    reset_gphoto_sensor_cache();
    constexpr int kCap = 200;  // a loop that never sees the deadline ends through the cap, never spins
    alpacacore::test::FakeTaskClock clock;
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_bulb_camera());
    fake.skip_sleeps = true;
    FakeRawDecoder decoder;

    // Runs on the exposure worker. Every hold slice is 100 ms of virtual time; record whether the shutter was
    // still open (one toggle so far) at each slice.
    int slices = 0;
    bool closed_during_hold = false;
    fake.before_call = [&](const std::string& name) {
        if (name != "drain_events") return;
        if (++slices > kCap) {
            throw AlpacaException("hold loop ran past the cap", alpacacore::AlpacaError::DriverException);
        }
        if (fake.bulb_toggle_history.size() != 1) closed_during_hold = true;
        clock.advance(std::chrono::milliseconds(100));
    };

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder, clock);
    driver->set_connected(true);
    driver->start_exposure(1.0, true);  // > 1/50 s native ceiling => bulb path
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_image_ready() == true);
    CHECK(slices == 10);  // 1 s of hold in 100 ms slices
    CHECK(fake.drained_budget == std::chrono::milliseconds(1000));
    CHECK(closed_during_hold == false);
    REQUIRE(fake.bulb_toggle_history.size() == 2);
    CHECK(fake.bulb_toggle_history.back() == false);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - bulb file wait deadline reads the TaskClock",
          "[gphoto][camera][unit][fakesdk][taskclock]") {
    reset_gphoto_sensor_cache();
    constexpr int kCap = 200;  // a loop that never sees the deadline ends through the cap, never spins
    alpacacore::test::FakeTaskClock clock;
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_bulb_camera());
    fake.skip_sleeps = true;
    fake.bulb_file_polls_before_ready = 1000;  // the camera never posts the file
    FakeRawDecoder decoder;

    int calls = 0;
    fake.before_call = [&](const std::string& name) {
        if (name != "drain_events" && name != "poll_bulb_file_and_download") return;
        if (++calls > kCap) {
            throw AlpacaException("bulb loops ran past the cap", alpacacore::AlpacaError::DriverException);
        }
        // Hold slices are 100 ms; the file poll slice is 1 s.
        clock.advance(name == "drain_events" ? std::chrono::milliseconds(100) : std::chrono::seconds(1));
    };

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder, clock);
    driver->set_connected(true);
    driver->start_exposure(1.0, true);
    wait_for_exposure_to_finish(*driver);

    // The file wait is the exposure plus 30 s: 31 one-second polls, then the refusal that names the window.
    CHECK(fake.bulb_file_polls == 31);
    require_exposure_failure_reported(*driver, "no file-added event from camera within 31 s");

    driver->set_connected(false);
}

// Mirror lock-up (MLU) and long-exposure noise reduction (LENR).
//
// With MLU on a Canon body times out in gp_camera_capture after 60 s, so the driver fires the shutter with two
// eosremoterelease press/release pairs instead: the first raises the mirror, the second takes the frame. The
// settle between them and every other wait read the TaskClock, so these cases spend no real time except the
// abort case, which needs the real clock to be interrupted.

namespace {

using alpacacore::vendor::gphoto::GPhotoOptions;

const std::vector<std::string> kPressReleasePressRelease{"Press Full", "Release Full", "Press Full", "Release Full"};

// A Canon body (mode dial M, eosremoterelease present) whose MLU menu item reads `value` through `widget`.
FakeGPhotoSDK::FakeCamera make_mlu_camera(const std::string& widget = "mirrorlock", const std::string& value = "1") {
    auto cam = make_canon_camera();
    cam.text_value[widget] = value;
    return cam;
}

int count_calls(const FakeGPhotoSDK& fake, const std::string& name) {
    return static_cast<int>(std::count(fake.call_log.begin(), fake.call_log.end(), name));
}

// Every WARN logged while it lives; restores the previous sink on any exit.
struct GPhotoWarnCapture {
    std::vector<std::string> messages;
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    GPhotoWarnCapture() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn) messages.emplace_back(message);
            });
    }
    ~GPhotoWarnCapture() { alpacacore::logging::set_log_sink(previous); }
    GPhotoWarnCapture(const GPhotoWarnCapture&) = delete;
    GPhotoWarnCapture& operator=(const GPhotoWarnCapture&) = delete;
    int count_containing(const std::string& text) const {
        return static_cast<int>(std::count_if(messages.begin(), messages.end(),
                                              [&](const std::string& m) { return m.find(text) != std::string::npos; }));
    }
};

}  // namespace

TEST_CASE("GPhoto camera fake - MLU off keeps the single capture path", "[gphoto][camera][unit][fakesdk][mlu]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_mlu_camera("mirrorlock", "0"));
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    const int captures_after_connect = count_calls(fake, "capture_and_download");
    const int reads_after_connect = count_calls(fake, "get_text_value");
    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_image_ready() == true);
    CHECK(count_calls(fake, "get_text_value") > reads_after_connect);  // StartExposure read MLU, and read it as off
    CHECK(count_calls(fake, "capture_and_download") == captures_after_connect + 1);
    CHECK(fake.remote_release_history.empty());
    CHECK(fake.bulb_file_polls == 0);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - MLU on runs press release settle press release on the TaskClock",
          "[gphoto][camera][unit][fakesdk][taskclock][mlu]") {
    const auto [widget, value] = GENERATE(std::pair<std::string, std::string>{"mirrorlock", "1"},
                                          std::pair<std::string, std::string>{"mirrorlockstatus", "1"},
                                          std::pair<std::string, std::string>{"mirrorlockstatus", "2"},
                                          std::pair<std::string, std::string>{"mirrorlock", "On"});
    CAPTURE(widget, value);
    reset_gphoto_sensor_cache();
    constexpr int kCap = 200;
    alpacacore::test::FakeTaskClock clock;
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_mlu_camera(widget, value));
    fake.skip_sleeps = true;
    FakeRawDecoder decoder;

    // Runs on the exposure worker: how many eosremoterelease writes had landed at each settle slice, and a
    // virtual-time slice per drain_events call.
    std::vector<std::size_t> writes_at_slice;
    fake.before_call = [&](const std::string& name) {
        if (name != "drain_events") return;
        if (writes_at_slice.size() >= kCap) {
            throw AlpacaException("settle loop ran past the cap", alpacacore::AlpacaError::DriverException);
        }
        writes_at_slice.push_back(fake.remote_release_history.size());
        clock.advance(std::chrono::milliseconds(100));
    };

    GPhotoOptions options;
    options.mirror_lockup_settle = std::chrono::milliseconds(2000);
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder, clock, options);
    driver->set_connected(true);
    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_image_ready() == true);
    CHECK(fake.remote_release_history == kPressReleasePressRelease);
    // 2 s of settle in 100 ms slices, every one between the first release and the second press.
    CHECK(writes_at_slice == std::vector<std::size_t>(20, 2));
    CHECK(fake.drained_budget == std::chrono::milliseconds(2000));
    CHECK(count_calls(fake, "capture_and_download") == 0);  // gp_camera_capture times out after 60 s with MLU on
    CHECK(fake.bulb_file_polls == 1);                       // the frame comes from the file-added event

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - MLU on bulb holds the second press for the exposure",
          "[gphoto][camera][unit][fakesdk][taskclock][mlu]") {
    reset_gphoto_sensor_cache();
    constexpr int kCap = 400;
    alpacacore::test::FakeTaskClock clock;
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_mlu_camera());
    fake.skip_sleeps = true;
    FakeRawDecoder decoder;

    std::vector<std::size_t> writes_at_slice;
    fake.before_call = [&](const std::string& name) {
        if (name != "drain_events") return;
        if (writes_at_slice.size() >= kCap) {
            throw AlpacaException("hold loop ran past the cap", alpacacore::AlpacaError::DriverException);
        }
        writes_at_slice.push_back(fake.remote_release_history.size());
        clock.advance(std::chrono::milliseconds(100));
    };

    GPhotoOptions options;
    options.mirror_lockup_settle = std::chrono::milliseconds(2000);
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder, clock, options);
    driver->set_connected(true);
    driver->start_exposure(1.0, true);  // > 1/50 s native ceiling => bulb path
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_image_ready() == true);
    CHECK(fake.remote_release_history == kPressReleasePressRelease);
    // 20 settle slices with two writes done, then 10 hold slices with the second press held.
    std::vector<std::size_t> expected(20, 2);
    expected.insert(expected.end(), 10, 3);
    CHECK(writes_at_slice == expected);
    CHECK(fake.drained_budget == std::chrono::milliseconds(3000));
    CHECK(count_calls(fake, "capture_and_download") == 0);
    CHECK(fake.bulb_file_polls == 1);
    CHECK(fake.choice_write_history["shutterspeed2"].back() == "bulb");

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - MLU on without a usable eosremoterelease refuses StartExposure",
          "[gphoto][camera][unit][fakesdk][mlu]") {
    reset_gphoto_sensor_cache();
    auto cam = make_mlu_camera();
    SECTION("widget absent") { cam.choices.erase("eosremoterelease"); }
    SECTION("widget present, no press/release pair") { cam.choices["eosremoterelease"] = {"None", "Press Half"}; }
    FakeGPhotoSDK fake;
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    REQUIRE(driver->get_connected() == true);

    try {
        driver->start_exposure(0.01, true);
        FAIL("StartExposure must refuse while MLU is on and the camera cannot fire twice");
    } catch (const AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::InvalidOperation);
        CHECK(std::string(e.what()).find("mirror lock-up") != std::string::npos);
        CHECK(std::string(e.what()).find("camera menu") != std::string::npos);
    }
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(count_calls(fake, "capture_and_download") == 0);
    CHECK(fake.choice_write_history.count("eosremoterelease") == 0);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - MLU on skips the connect priming capture", "[gphoto][camera][unit][fakesdk][mlu]") {
    // The priming capture is a gp_camera_capture, which stalls 60 s with MLU on. Geometry is learned from the
    // first real exposure instead, as when priming fails.
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_mlu_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    CHECK(driver->get_connected() == true);
    CHECK(count_calls(fake, "capture_and_download") == 0);
    CHECK(fake.remote_release_history.empty());

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - an unreadable MLU state at connect skips the priming capture quietly",
          "[gphoto][camera][unit][fakesdk][mlu]") {
    // Falsified by: initialise skip_priming to false in the connect path (an unreadable state then runs the
    // priming capture, which stalls 60 s with MLU on). Priming is best-effort, so the failed read neither
    // fails the connect nor logs a warning.
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_mlu_camera());
    FakeRawDecoder decoder;
    fake.before_call = [&](const std::string& name) {
        if (name == "get_text_value") {
            throw alpacacore::AlpacaException("mirrorlock read failed", alpacacore::AlpacaError::DriverException);
        }
    };

    GPhotoWarnCapture warnings;
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    CHECK(driver->get_connected() == true);
    CHECK(count_calls(fake, "capture_and_download") == 0);
    CHECK(warnings.messages.empty());

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - the MLU exposure deadline includes the settle and the file wait",
          "[gphoto][camera][unit][fakesdk][taskclock][mlu]") {
    reset_gphoto_sensor_cache();
    alpacacore::test::FakeTaskClock clock;
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_mlu_camera());
    fake.skip_sleeps = true;
    FakeRawDecoder decoder;

    GPhotoOptions options;
    options.mirror_lockup_settle = std::chrono::milliseconds(10000);
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder, clock, options);
    driver->set_connected(true);

    // Deadline = 0.01 s + 60 s + 10 s settle + (0.01 s + 30 s) file wait = 100.02 s. Without the settle it is
    // 90.02 s and without the file wait 70.01 s; each would already have declared a valid MLU exposure Idle at
    // the first check below that it does not reach.
    fake.bulb_file_polls_before_ready = 1;
    alpacacore::CameraState at_65s = alpacacore::CameraState::Idle;
    alpacacore::CameraState at_95s = alpacacore::CameraState::Idle;
    alpacacore::CameraState at_101s = alpacacore::CameraState::Exposing;
    bool settled = false;
    int polls = 0;
    fake.before_call = [&](const std::string& name) {
        if (name == "drain_events" && !settled) {
            settled = true;
            clock.advance(std::chrono::seconds(65));
            at_65s = driver->get_camera_state();
        } else if (name == "poll_bulb_file_and_download") {
            if (++polls == 1) {
                clock.advance(std::chrono::seconds(30));
                at_95s = driver->get_camera_state();
            } else if (polls == 2) {
                clock.advance(std::chrono::seconds(6));
                at_101s = driver->get_camera_state();
            }
        }
    };
    driver->start_exposure(0.01, true);
    for (int i = 0; i < 500 && polls < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(polls >= 2);

    CHECK(at_65s == alpacacore::CameraState::Exposing);
    CHECK(at_95s == alpacacore::CameraState::Exposing);
    CHECK(at_101s == alpacacore::CameraState::Idle);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - an abort during the MLU settle still fires the second press and release",
          "[gphoto][camera][unit][fakesdk][mlu]") {
    // The mirror is up after the first pair. Stopping there would leave the body to fire on the NEXT exposure's
    // first press, so the sequence is finished, the frame is discarded and the settle is cut short.
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_mlu_camera());
    FakeRawDecoder decoder;

    std::atomic<bool> settling{false};
    fake.before_call = [&](const std::string& name) {
        if (name == "drain_events") settling.store(true);
    };
    GPhotoOptions options;
    options.mirror_lockup_settle = std::chrono::milliseconds(10000);  // the real clock: only the abort ends it
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder,
                                                                   alpacacore::util::default_task_clock(), options);
    driver->set_connected(true);
    driver->start_exposure(0.01, true);
    for (int i = 0; i < 500 && !settling.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(settling.load());

    const auto started = std::chrono::steady_clock::now();
    driver->abort_exposure();  // joins the worker
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));

    CHECK(fake.remote_release_history == kPressReleasePressRelease);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_image_ready() == false);
    CHECK(driver->get_connected() == true);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - a refused press in the MLU sequence is reported once and not retried",
          "[gphoto][camera][unit][fakesdk][mlu]") {
    reset_gphoto_sensor_cache();
    alpacacore::test::FakeTaskClock clock;
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_mlu_camera());
    fake.skip_sleeps = true;
    FakeRawDecoder decoder;
    int press_attempts = 0;
    fake.before_call = [&](const std::string& name) {
        if (name == "drain_events") clock.advance(std::chrono::milliseconds(100));
        if (name == "set_choice_value") ++press_attempts;
    };

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder, clock);
    driver->set_connected(true);
    fake.refuse_choice_writes.push_back("eosremoterelease");
    press_attempts = 0;
    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    require_exposure_failure_reported(*driver, "Failed to set eosremoterelease = Press Full");
    CHECK(press_attempts == 2);  // the shutter-speed write, then the one refused press
    CHECK(count_calls(fake, "capture_and_download") == 0);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - MLU on beside a bulb toggle still fires the eosremoterelease pair",
          "[gphoto][camera][unit][fakesdk][taskclock][mlu]") {
    // A Canon body that lists a "bulb" widget as well as eosremoterelease: the pair is still found, so MLU is
    // not refused, and the shutter is fired with the pair, never the bulb toggle.
    reset_gphoto_sensor_cache();
    alpacacore::test::FakeTaskClock clock;
    FakeGPhotoSDK fake;
    auto cam = make_mlu_camera();
    cam.toggle_value["bulb"] = false;
    fake.cameras.push_back(cam);
    fake.skip_sleeps = true;
    FakeRawDecoder decoder;
    fake.before_call = [&](const std::string& name) {
        if (name == "drain_events") clock.advance(std::chrono::milliseconds(100));
    };

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder, clock);
    driver->set_connected(true);
    driver->start_exposure(1.0, true);  // > 1/50 s native ceiling => bulb path
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_image_ready() == true);
    CHECK(fake.remote_release_history == kPressReleasePressRelease);
    CHECK(fake.bulb_toggle_history.empty());
    CHECK(count_calls(fake, "capture_and_download") == 0);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - longExposureNR writes longexpnr once at connect",
          "[gphoto][camera][unit][fakesdk][lenr]") {
    reset_gphoto_sensor_cache();
    const auto [mode, expected] =
        GENERATE(std::pair<std::string, std::string>{"off", "Off"}, std::pair<std::string, std::string>{"on", "On"});
    CAPTURE(mode);
    FakeGPhotoSDK fake;
    auto cam = make_camera();
    cam.choices["longexpnr"] = {"On", "Off"};
    cam.choice_value["longexpnr"] = "On";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    GPhotoOptions options;
    options.long_exposure_nr = mode;
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder,
                                                                   alpacacore::util::default_task_clock(), options);
    driver->set_connected(true);
    REQUIRE(fake.choice_write_history["longexpnr"].size() == 1);
    CHECK(fake.choice_write_history["longexpnr"].front() == expected);

    // Never again, and never during an exposure.
    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);
    CHECK(fake.choice_write_history["longexpnr"].size() == 1);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - longExposureNR matches the camera's choice text without regard to case",
          "[gphoto][camera][unit][fakesdk][lenr]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_camera();
    cam.choices["longexpnr"] = {"ON", "OFF"};
    cam.choice_value["longexpnr"] = "OFF";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    GPhotoOptions options;
    options.long_exposure_nr = "on";
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder,
                                                                   alpacacore::util::default_task_clock(), options);
    driver->set_connected(true);
    REQUIRE(fake.choice_write_history["longexpnr"].size() == 1);
    CHECK(fake.choice_write_history["longexpnr"].front() == "ON");

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - longExposureNR camera writes nothing", "[gphoto][camera][unit][fakesdk][lenr]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_camera();
    cam.choices["longexpnr"] = {"On", "Off"};
    cam.choice_value["longexpnr"] = "On";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    GPhotoWarnCapture warnings;
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);  // default: "camera"
    driver->set_connected(true);

    CHECK(fake.choice_write_history.count("longexpnr") == 0);
    CHECK(fake.cameras[0].choice_value["longexpnr"] == "On");
    CHECK(warnings.count_containing("long-exposure NR") == 0);  // "camera" is not a request, so nothing to warn about

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - longExposureNR on a body without the widget warns once and connects",
          "[gphoto][camera][unit][fakesdk][lenr]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_canon_camera());  // no longexpnr, as on every Canon tested
    FakeRawDecoder decoder;

    GPhotoWarnCapture warnings;
    GPhotoOptions options;
    options.long_exposure_nr = "off";
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder,
                                                                   alpacacore::util::default_task_clock(), options);
    driver->set_connected(true);

    CHECK(driver->get_connected() == true);
    CHECK(fake.choice_write_history.count("longexpnr") == 0);
    CHECK(warnings.count_containing("long-exposure NR is not settable on this camera; set it in the camera menu") == 1);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - a refused longexpnr write warns and does not fail connect",
          "[gphoto][camera][unit][fakesdk][lenr]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_camera();
    cam.choices["longexpnr"] = {"On", "Off"};
    cam.choice_value["longexpnr"] = "On";
    fake.cameras.push_back(cam);
    fake.refuse_choice_writes.push_back("longexpnr");
    FakeRawDecoder decoder;

    GPhotoWarnCapture warnings;
    GPhotoOptions options;
    options.long_exposure_nr = "off";
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder,
                                                                   alpacacore::util::default_task_clock(), options);
    driver->set_connected(true);

    CHECK(driver->get_connected() == true);
    CHECK(fake.close_count == 0);
    CHECK(warnings.count_containing("long-exposure NR") == 1);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - longExposureNR with no matching choice warns and writes nothing",
          "[gphoto][camera][unit][fakesdk][lenr]") {
    // Falsified by: delete the match == choices.end() guard in apply_long_exposure_nr_locked (the write then
    // dereferences the end iterator).
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_camera();
    cam.choices["longexpnr"] = {"Enable", "Disable"};
    cam.choice_value["longexpnr"] = "Enable";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    GPhotoWarnCapture warnings;
    GPhotoOptions options;
    options.long_exposure_nr = "off";
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder,
                                                                   alpacacore::util::default_task_clock(), options);
    driver->set_connected(true);

    CHECK(driver->get_connected() == true);
    CHECK(fake.choice_write_history.count("longexpnr") == 0);
    CHECK(warnings.count_containing("long-exposure NR has no \"off\" choice on this camera") == 1);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - a camera gone from the USB bus during the longexpnr write fails the connect",
          "[gphoto][camera][unit][fakesdk][lenr]") {
    // Falsified by: remove the GPhotoDeviceGoneException rethrow in apply_long_exposure_nr_locked (the catch
    // of std::exception then logs a WARN and the connect succeeds on a dead handle).
    const std::string call = GENERATE(as<std::string>{}, "has_widget", "get_choices", "set_choice_value");
    CAPTURE(call);
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_camera();
    cam.choices["longexpnr"] = {"On", "Off"};
    cam.choice_value["longexpnr"] = "On";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    // Keyed on the widget name, not on a call count: only the longexpnr step touches that widget, so the fault
    // lands in it however many other calls (a priming capture, a lost sensor-cache line) a connect makes.
    const int opened_before = fake.open_count;
    const int closed_before = fake.close_count;
    int hits = 0;
    fake.before_call = [&](const std::string& name) {
        if (name == call && fake.current_widget == "longexpnr") {
            ++hits;
            throw alpacacore::vendor::gphoto::GPhotoDeviceGoneException("Could not find the requested device",
                                                                        alpacacore::AlpacaError::DriverException);
        }
    };
    GPhotoOptions options;
    options.long_exposure_nr = "off";
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder,
                                                                   alpacacore::util::default_task_clock(), options);
    CHECK_THROWS(driver->set_connected(true));

    CHECK(hits == 1);
    CHECK(driver->get_connected() == false);
    CHECK(fake.open_count - opened_before == 1);
    CHECK(fake.close_count - closed_before == 1);
}

TEST_CASE("GPhoto camera fake - MLU read finding the camera gone from the USB bus reports a disconnect",
          "[gphoto][camera][unit][fakesdk][mlu]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_mlu_camera("mirrorlock", "0"));
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    REQUIRE(driver->get_connected() == true);

    std::atomic<bool> armed{true};
    fake.before_call = [&](const std::string& name) {
        if (armed.load() && name == "has_widget") {
            throw alpacacore::vendor::gphoto::GPhotoDeviceGoneException("Could not find the requested device",
                                                                        alpacacore::AlpacaError::DriverException);
        }
    };
    require_alpaca_error([&] { driver->start_exposure(0.01, true); }, alpacacore::AlpacaError::NotConnected);

    // The body left the bus: the client must see Connected false and reconnect.
    CHECK(driver->get_connected() == false);
    CHECK(fake.close_count == fake.open_count);

    // Reconnect enumerates again and the next exposure works.
    armed.store(false);
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);
    CHECK(driver->get_image_ready() == true);

    driver->set_connected(false);
    CHECK(fake.close_count == fake.open_count);
}

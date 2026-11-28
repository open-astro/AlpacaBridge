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

// The remotealpaca Camera proxied device (decision 0023, issue #1753 slice 5, #2703) over the scripted FakeHttpClient
// and a scripted upstream (alpaca_upstream_script.h), with the poll on the fake task clock (decision 0005). Fake
// evidence only: no socket, no network. Cases 3 and 4 (and part of 6) of the required set come from the driver's
// contract_sweep.h entry. The rules the Camera shares with the other proxied devices (the snapshot, the loss bound,
// the self-loop check) live in UpstreamProxy (upstream_proxy.h) and are covered at length by
// test_remotealpaca_observingconditions.cpp; the cases here pin what the Camera's own members do with each of them,
// and the client's ImageBytes download (remote_alpaca_client.h rule 6).

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_camera_driver.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "alpaca_upstream_script.h"
#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_http_client.h"
#include "fake_task_clock.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
namespace remote = alpacacore::vendor::remotealpaca;
using alpacacore::CameraDriver;
using alpacacore::CameraState;
using alpacacore::test::alpaca_error_message;
using alpacacore::test::AlpacaUpstreamScript;
using alpacacore::test::error_code_of;
using alpacacore::test::FakeHttpClient;
using alpacacore::test::require_alpaca_error;

constexpr std::chrono::milliseconds kRendezvous{5000};
constexpr int kUpstreamPort = 11111;
const std::string kPrefix = "127.0.0.1:11111: ";

// The fake clock, the scripted upstream behind the fake HTTP client, and the driver, destroyed in reverse.
struct Rig {
    alpacacore::test::FakeTaskClock clock;
    FakeHttpClient http;
    AlpacaUpstreamScript upstream{AlpacaUpstreamScript::Kind::Camera};
    std::unique_ptr<CameraDriver> driver;

    explicit Rig(remote::ServerAddressSource self = {}, int device_number = 0) {
        upstream.install(http);
        driver = remote::create_remotealpaca_camera(device_number, config(std::chrono::seconds(1)), http.client(),
                                                    std::move(self), clock);
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    ~Rig() { driver.reset(); }

    static remote::RemoteDeviceConfig config(std::chrono::seconds poll) {
        remote::RemoteDeviceConfig config;
        config.host = "127.0.0.1";
        config.port = kUpstreamPort;
        config.upstream_device_number = 0;
        config.poll_period = poll;
        return config;
    }

    // Connect, then wait for the poll to park on the clock.
    void connect() {
        REQUIRE(alpacacore::test::connect_and_park(*driver, clock, kRendezvous));
        REQUIRE(driver->get_connected());
    }

    // One poll period on the clock; returns when the woken poll has parked again or ended.
    void poll(std::chrono::nanoseconds by = std::chrono::seconds(1)) {
        clock.advance(by);
        REQUIRE(clock.wait_for_woken_settled(kRendezvous));
    }

    // Three failed polls in a row latch the fault (decision 0009 rule 1); the upstream answers again after them.
    void fault() {
        http.fail_next(3);
        poll();
        poll();
        poll();
        REQUIRE(driver->get_connected());
        REQUIRE_FALSE(driver->get_link_fault().empty());
    }

    // The members the upstream was sent as PUTs, in order.
    std::vector<std::string> puts() const {
        std::vector<std::string> out;
        for (const auto& request : http.requests()) {
            if (request.method == "PUT") out.push_back(request.url.substr(request.url.rfind('/') + 1));
        }
        return out;
    }
};

// The image the scripted upstream reads out for a NumX x NumY frame, checked in this server's row-major layout.
void check_image(const alpacacore::ImageArray& image, int width, int height) {
    REQUIRE(image.width == width);
    REQUIRE(image.height == height);
    REQUIRE(image.rank == 2);
    REQUIRE(image.data.size() == static_cast<std::size_t>(width * height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            INFO("x " << x << " y " << y);
            CHECK(image.data[static_cast<std::size_t>(y * width + x)] == AlpacaUpstreamScript::pixel(x, y));
        }
    }
}

// An ImageBytes reply built by hand: the metadata (version 1), then `data`.
std::string image_bytes(std::uint32_t error, std::uint32_t transmission, std::uint32_t rank, std::uint32_t dim1,
                        std::uint32_t dim2, std::uint32_t dim3, const std::string& data) {
    std::string body;
    const auto le32 = [&body](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) body += static_cast<char>((value >> shift) & 0xFF);
    };
    for (const std::uint32_t field : {1u, error, 0u, 0u, 44u, 2u, transmission, rank, dim1, dim2, dim3}) le32(field);
    return body + data;
}

double nan() { return std::numeric_limits<double>::quiet_NaN(); }

}  // namespace

TEST_CASE("Remote Alpaca Camera Driver - Defaults", "[remotealpaca][camera][unit]") {
    Rig rig;
    REQUIRE(rig.driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(rig.driver->get_device_number() == 0);
    REQUIRE_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_connecting());
    CHECK(rig.driver->get_name() == "Remote Alpaca Camera");
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_device_state().empty());
    // The capabilities answer while disconnected: false before any Connect has read the upstream's.
    CHECK_FALSE(rig.driver->get_can_abort_exposure());
    CHECK_FALSE(rig.driver->get_can_stop_exposure());
    CHECK_FALSE(rig.driver->get_can_pulse_guide());
    CHECK_FALSE(rig.driver->get_can_asymmetric_bin());
    CHECK_FALSE(rig.driver->get_can_fast_readout());
    CHECK_FALSE(rig.driver->get_can_get_cooler_power());
    CHECK_FALSE(rig.driver->get_can_set_ccd_temperature());
    // The sensor is the upstream's, read at Connect, so it is NotConnected.
    require_alpaca_error([&] { (void)rig.driver->get_camera_x_size(); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);  // building the driver touches no network
}

TEST_CASE("Remote Alpaca Camera Driver - Device metadata", "[remotealpaca][camera][unit]") {
    Rig rig({}, 3);
    CHECK(rig.driver->get_device_number() == 3);
    CHECK(rig.driver->get_description() == "Camera of the upstream Alpaca server 127.0.0.1:11111 device 0");
    CHECK(rig.driver->get_driver_info() == "AlpacaCore Remote Alpaca Camera Driver");
    CHECK(rig.driver->get_driver_version() == alpacacore::kVersion);
    CHECK(rig.driver->get_interface_version() == 4);
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_CAM_3");
    // Decision 0023 rule 5: the UniqueID is this server's own, never the upstream's, connected or not.
    rig.upstream.set("uniqueid", "\"upstream-unique-id-0000\"");
    rig.connect();
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_CAM_3");
    CHECK(rig.upstream.calls("uniqueid") == 0);
    // Once a Connect read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    CHECK(rig.driver->get_name() == "Remote Alpaca Alpaca Simulator Camera");
    CHECK(rig.driver->get_description() == "ASCOM Alpaca Simulator Camera");
    CHECK(rig.driver->get_sensor_name() == "IMX571");
    CHECK(rig.driver->get_sensor_type() == alpacacore::SensorType::Monochrome);
}

TEST_CASE("Remote Alpaca Camera Driver - the getters serve the snapshot and the Connect's reads, calling nothing",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    const int calls_after_connect = rig.http.calls();
    CHECK(c.get_camera_x_size() == 8);
    CHECK(c.get_camera_y_size() == 6);
    CHECK(c.get_max_bin_x() == 2);
    CHECK(c.get_pixel_size_x() == 3.76);
    CHECK(c.get_exposure_min() == 0.001);
    CHECK(c.get_exposure_max() == 3600.0);
    CHECK(c.get_gain_min() == 0);
    CHECK(c.get_gain_max() == 100);
    CHECK(c.get_readout_modes() == std::vector<std::string>{"Normal", "Fast"});
    CHECK(c.get_max_adu() == 65535);
    CHECK(c.get_can_abort_exposure());
    CHECK_FALSE(c.get_can_asymmetric_bin());
    CHECK(c.get_bin_x() == 1);
    CHECK(c.get_num_x() == 8);
    CHECK(c.get_num_y() == 6);
    CHECK(c.get_gain() == 10);
    CHECK(c.get_offset() == 5);
    CHECK_FALSE(c.get_cooler_on());
    CHECK(c.get_set_ccd_temperature() == -10.0);
    CHECK(c.get_camera_state() == CameraState::Idle);
    CHECK_FALSE(c.get_image_ready());
    CHECK(c.get_ccd_temperature() == 20.0);
    CHECK(c.get_cooler_power() == 0.0);
    CHECK_FALSE(c.get_is_pulse_guiding());
    const auto state = c.get_device_state();
    // CameraState, CCDTemperature, CoolerPower, ImageReady, IsPulseGuiding and TimeStamp: no HeatSinkTemperature (the
    // upstream lacks it) and no PercentCompleted (not exposing).
    REQUIRE(state.size() == 6);
    CHECK(state[0].name == "CameraState");
    CHECK(std::get<std::int32_t>(state[0].value) == 0);
    CHECK(state[1].name == "CCDTemperature");
    CHECK(state.back().name == "TimeStamp");
    CHECK(rig.http.calls() == calls_after_connect);

    // The poll asks CameraState, ImageReady, CCDTemperature, CoolerPower and IsPulseGuiding: five calls, never the
    // sensor, the settings, HeatSinkTemperature (missing at Connect) or LastExposure* (no new image).
    rig.upstream.set("ccdtemperature", "-5");
    rig.upstream.set("cameraxsize", "16");
    rig.upstream.set("gain", "50");
    CHECK(c.get_ccd_temperature() == 20.0);  // until the next poll the frame holds
    rig.poll();
    CHECK(rig.http.calls() == calls_after_connect + 5);
    CHECK(c.get_ccd_temperature() == -5.0);
    CHECK(c.get_camera_x_size() == 8);
    CHECK(c.get_gain() == 10);
    CHECK(rig.upstream.calls("cameraxsize") == 1);
    CHECK(rig.upstream.calls("heatsinktemperature") == 1);
    CHECK(rig.upstream.calls("lastexposureduration") == 1);  // the Connect's first frame only
}

// DeviceState's TimeStamp is when the frame was measured (decision 0010), through the camera base's
// device_state_age(): a frame the poll stopped refreshing carries its age.
TEST_CASE("Remote Alpaca Camera Driver - DeviceState is stamped with the frame's age", "[remotealpaca][camera][unit]") {
    Rig rig;
    rig.connect();
    remote::set_poll_paused_for_testing(*rig.driver, true);
    rig.poll(std::chrono::seconds(20));
    const auto stamp = alpacacore::test::parse_timestamp(alpacacore::test::time_stamp_of(*rig.driver));
    const auto age = std::chrono::system_clock::now() - stamp;
    CHECK(age >= std::chrono::seconds(19));
    CHECK(age <= std::chrono::seconds(25));
    remote::set_poll_paused_for_testing(*rig.driver, false);
}

TEST_CASE("Remote Alpaca Camera Driver - StartExposure reads Exposing at once, and only the poll reads ImageReady true",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    c.start_exposure(2.5, false);
    CHECK(rig.upstream.last_start() == "2.5 false");
    // Written through: Exposing, not ready, 0 % before any poll.
    CHECK(c.get_camera_state() == CameraState::Exposing);
    CHECK_FALSE(c.get_image_ready());
    CHECK(c.get_percent_completed_checked() == 0.0);
    rig.upstream.set("percentcompleted", "40");
    rig.poll();
    CHECK(c.get_camera_state() == CameraState::Exposing);
    CHECK(c.get_percent_completed_checked() == 40.0);
    // The upstream finished: the frame says so only at the next poll.
    rig.upstream.finish_exposure();
    CHECK_FALSE(c.get_image_ready());
    rig.poll();
    CHECK(c.get_image_ready());
    CHECK(c.get_camera_state() == CameraState::Idle);
    require_alpaca_error([&] { (void)c.get_percent_completed_checked(); }, AlpacaError::InvalidOperation);
    // A duration below ExposureMin, 0 (a bias frame) included, runs at ExposureMin (camera argument order step 3).
    c.start_exposure(0.0, true);
    CHECK(rig.upstream.last_start() == "0.001 true");
    // The next StartExposure reads ImageReady false at once again: a ready frame a poll period old says nothing about
    // the new exposure.
    CHECK_FALSE(c.get_image_ready());
}

TEST_CASE("Remote Alpaca Camera Driver - ImageArray is fetched as ImageBytes, decoded row-major, then held",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    c.set_num_x(5);  // an asymmetric frame: 5 x 6 shows a transposed decode
    c.start_exposure(1.0, true);
    rig.upstream.finish_exposure();
    rig.poll();
    REQUIRE(c.get_image_ready());
    check_image(c.get_image_array(), 5, 6);
    REQUIRE(rig.upstream.calls("imagearray") == 1);
    // The download asked for ImageBytes, with a body cap for the full 8 x 6 frame and a timeout past the 5 s per-call
    // one: the bounds image_fetch_bounds() gives the sensor the Connect read.
    const remote::ImageFetchBounds bounds = remote::image_fetch_bounds(8, 6, 1);
    bool asked = false;
    for (const auto& request : rig.http.requests()) {
        if (request.url.find("/imagearray") == std::string::npos) continue;
        asked = true;
        CHECK(request.accept == "application/imagebytes");
        CHECK(request.max_body_bytes == bounds.max_body_bytes);
        CHECK(request.timeout == bounds.timeout);
    }
    CHECK(asked);
    // Held: a second read calls nothing; a new exposure drops it.
    check_image(c.get_image_array(), 5, 6);
    CHECK(rig.upstream.calls("imagearray") == 1);
    c.start_exposure(1.0, true);
    require_alpaca_error([&] { (void)c.get_image_array(); }, AlpacaError::InvalidOperation);
    CHECK(rig.upstream.calls("imagearray") == 2);
}

TEST_CASE("Remote Alpaca Camera Driver - ImageArray and LastExposure before any image are asked too early",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    // The upstream's ImageBytes error reply: its number unchanged, its text with the host in front, flagged asked too
    // early so the router logs it at DEBUG (issue #1167).
    try {
        (void)c.get_image_array();
        FAIL("ImageArray before any image must throw");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::InvalidOperation);
        CHECK(e.is_asked_too_early());
        CHECK(std::string(e.what()) == kPrefix + "No image is ready");
    }
    for (const bool start_time : {false, true}) {
        try {
            if (start_time) {
                (void)c.get_last_exposure_start_time();
            } else {
                (void)c.get_last_exposure_duration();
            }
            FAIL("LastExposure* before any image must throw");
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == AlpacaError::InvalidOperation);
            CHECK(e.is_asked_too_early());
        }
    }
    CHECK(c.get_connected());
    CHECK(c.get_link_fault().empty());
}

TEST_CASE("Remote Alpaca Camera Driver - an upstream that answers ImageArray in JSON is refused, its error passes",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    rig.upstream.set_image_json(true);
    rig.connect();
    auto& c = *rig.driver;
    // A JSON error reply passes through unchanged.
    CHECK(alpaca_error_message([&] { (void)c.get_image_array(); }, AlpacaError::InvalidOperation) ==
          kPrefix + "No image is ready");
    // A JSON image is no usable reply: a full frame as JSON text is too large to parse here. The link stays up.
    c.start_exposure(1.0, true);
    rig.upstream.finish_exposure();
    rig.poll();
    CHECK(alpaca_error_message([&] { (void)c.get_image_array(); }, AlpacaError::DriverException)
              .find("is a JSON image, not ImageBytes") != std::string::npos);
    CHECK(c.get_connected());
    CHECK(c.get_link_fault().empty());
}

// remote_alpaca_client.h rule 6: the download takes only its transaction ID under the client's call mutex, so the poll
// keeps refreshing the frame while a long image arrives, and the frame never ages to a lost link from a download.
TEST_CASE("Remote Alpaca Camera Driver - the poll runs while the image downloads", "[remotealpaca][camera][unit]") {
    Rig rig;
    alpacacore::test::Latch download;
    std::atomic<bool> downloading{false};
    std::atomic<bool> polled_during{false};
    rig.http.set_responder([&](const FakeHttpClient::Request& request) {
        const bool image = request.url.find("/imagearray") != std::string::npos;
        if (image) {
            downloading.store(true);
            download.block();
        } else if (downloading.load() && request.url.find("/camerastate") != std::string::npos) {
            polled_during.store(true);
        }
        auto response = rig.upstream.answer(request);
        if (image) downloading.store(false);
        return response;
    });
    rig.connect();
    auto& c = *rig.driver;
    c.start_exposure(1.0, true);
    rig.upstream.finish_exposure();
    rig.poll();
    int code = -2;
    std::thread reader([&] { code = alpacacore::test::worker_code_of([&] { (void)c.get_image_array(); }); });
    CHECK(download.wait_entered());  // CHECK, not REQUIRE: the thread is joined below either way
    rig.clock.advance(std::chrono::seconds(1));
    (void)rig.clock.wait_for_woken_settled(kRendezvous);
    CHECK(polled_during.load());
    download.release();
    reader.join();
    CHECK(code == 0);
}

// The upstream is a shared Alpaca server: another of its clients may expose too. A poll that reads ImageReady false
// drops the held image, so once the other client's exposure reads ready, the next read downloads its image, not the
// one held from before.
TEST_CASE("Remote Alpaca Camera Driver - an exposure another upstream client takes replaces the held image",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    c.start_exposure(1.0, true);
    rig.upstream.finish_exposure();
    rig.poll();
    check_image(c.get_image_array(), 8, 6);  // held
    REQUIRE(rig.upstream.calls("imagearray") == 1);
    // The other client: a 4-pixel-wide frame and an exposure, sent to the upstream, not through this server.
    const auto other_put = [&](const std::string& member, const std::string& body) {
        FakeHttpClient::Request request;
        request.method = "PUT";
        request.url = "http://127.0.0.1:11111/api/v1/camera/0/" + member;
        request.form_body = body + "&ClientID=99&ClientTransactionID=1";
        (void)rig.upstream.answer(request);
    };
    other_put("numx", "NumX=4");
    other_put("startexposure", "Duration=1&Light=True");
    rig.poll();
    CHECK_FALSE(c.get_image_ready());
    rig.upstream.finish_exposure();
    rig.poll();
    REQUIRE(c.get_image_ready());
    check_image(c.get_image_array(), 4, 6);
    CHECK(rig.upstream.calls("imagearray") == 2);
}

// Two readers of one exposure: the second queues behind the first's download (the command order) and takes the image
// the first held, so the frame is downloaded once.
TEST_CASE("Remote Alpaca Camera Driver - a second ImageArray reader takes the image the first downloaded",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    alpacacore::test::Latch download;
    rig.http.set_responder([&](const FakeHttpClient::Request& request) {
        if (request.url.find("/imagearray") != std::string::npos) download.block();
        return rig.upstream.answer(request);
    });
    rig.connect();
    auto& c = *rig.driver;
    c.start_exposure(1.0, true);
    rig.upstream.finish_exposure();
    rig.poll();
    alpacacore::ImageArray first;
    alpacacore::ImageArray second;
    int first_code = -2;
    int second_code = -2;
    std::thread reader([&] { first_code = alpacacore::test::worker_code_of([&] { first = c.get_image_array(); }); });
    CHECK(download.wait_entered());  // CHECK, not REQUIRE: the threads are joined below either way
    alpacacore::test::Latch second_waits;
    remote::set_command_waiting_hook_for_testing(c, [&] { second_waits.block(); });
    std::thread other([&] { second_code = alpacacore::test::worker_code_of([&] { second = c.get_image_array(); }); });
    CHECK(second_waits.wait_entered());
    second_waits.release();
    download.release();
    reader.join();
    other.join();
    CHECK(first_code == 0);
    CHECK(second_code == 0);
    check_image(first, 8, 6);
    check_image(second, 8, 6);
    CHECK(rig.upstream.calls("imagearray") == 1);
}

TEST_CASE("Remote Alpaca Camera Driver - LastExposure is read once per image and a new exposure keeps the last one",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    c.start_exposure(2.0, true);
    rig.upstream.finish_exposure();
    rig.poll();
    REQUIRE(c.get_image_ready());
    CHECK(c.get_last_exposure_duration() == 2.0);
    const auto start = c.get_last_exposure_start_time();
    CHECK(alpacacore::test::parse_timestamp("2026-11-26T12:00:00.500Z") == start);
    const int reads = rig.upstream.calls("lastexposureduration");
    rig.poll();
    rig.poll();
    CHECK(rig.upstream.calls("lastexposureduration") == reads);  // ImageReady stayed true: nothing new to read
    // A new exposure, aborted: no image, so the last success still answers (a failed exposure never erases it).
    c.start_exposure(5.0, true);
    c.abort_exposure();
    rig.poll();
    CHECK(c.get_camera_state() == CameraState::Idle);
    CHECK_FALSE(c.get_image_ready());
    CHECK(c.get_last_exposure_duration() == 2.0);
    // The next image is read again.
    c.start_exposure(3.0, true);
    rig.upstream.finish_exposure();
    rig.poll();
    CHECK(c.get_last_exposure_duration() == 3.0);
    CHECK(rig.upstream.calls("lastexposureduration") == reads + 1);
}

TEST_CASE("Remote Alpaca Camera Driver - AbortExposure and StopExposure are forwarded while faulted, others are not",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    c.start_exposure(30.0, true);
    rig.fault();
    // Faulted: the frame is refused with DriverException; the Connect's reads still answer.
    CHECK(alpaca_error_message([&] { (void)c.get_camera_state(); }, AlpacaError::DriverException)
              .find("communications compromised") != std::string::npos);
    require_alpaca_error([&] { (void)c.get_image_ready(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)c.get_ccd_temperature(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)c.get_image_array(); }, AlpacaError::DriverException);
    CHECK(c.get_camera_x_size() == 8);
    CHECK(c.get_gain() == 10);
    CHECK(c.get_can_abort_exposure());
    // The stops go out and read CameraState back; the commands that need the frame are refused and send nothing.
    const std::size_t puts_before = rig.puts().size();
    c.stop_exposure();
    c.abort_exposure();
    CHECK(rig.upstream.calls("stopexposure") == 1);
    CHECK(rig.upstream.calls("abortexposure") == 1);
    require_alpaca_error([&] { c.start_exposure(1.0, true); }, AlpacaError::DriverException);
    require_alpaca_error([&] { c.set_bin_x(2); }, AlpacaError::DriverException);
    require_alpaca_error([&] { c.set_gain(20); }, AlpacaError::DriverException);
    require_alpaca_error([&] { c.set_cooler_on(true); }, AlpacaError::DriverException);
    require_alpaca_error([&] { c.pulse_guide(0, 100); }, AlpacaError::DriverException);
    CHECK(rig.puts().size() == puts_before + 2);
    CHECK(rig.upstream.calls("imagearray") == 0);
    // The next good poll clears the fault: the stopped exposure read out its image.
    rig.poll();
    CHECK(c.get_link_fault().empty());
    CHECK(c.get_camera_state() == CameraState::Idle);
    CHECK(c.get_image_ready());
}

// Commands reach the upstream in caller order (UpstreamProxy's command order mutex): an AbortExposure sent while a
// StartExposure's body is held before its PUT waits for that start, so the camera never exposes after the abort.
TEST_CASE("Remote Alpaca Camera Driver - an AbortExposure sent during a StartExposure reaches the upstream after it",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    rig.connect();
    alpacacore::test::Latch start_body;
    std::atomic<bool> held{false};  // only the first body (the start's) holds
    remote::set_command_hook_for_testing(*rig.driver, [&] {
        if (!held.exchange(true)) start_body.block();
    });
    std::thread starter([&] { rig.driver->start_exposure(10.0, true); });
    REQUIRE(start_body.wait_entered());
    // The abort's caller finds the start's command running and waits for it: the hook marks the moment.
    alpacacore::test::Latch abort_waits;
    remote::set_command_waiting_hook_for_testing(*rig.driver, [&] { abort_waits.block(); });
    std::thread aborter([&] { rig.driver->abort_exposure(); });
    CHECK(abort_waits.wait_entered());  // CHECK, not REQUIRE: the threads are joined below either way
    CHECK(rig.upstream.calls("abortexposure") == 0);
    abort_waits.release();
    start_body.release();
    starter.join();
    aborter.join();
    const auto puts = rig.puts();
    REQUIRE(puts.size() >= 2);
    CHECK(puts[puts.size() - 2] == "startexposure");
    CHECK(puts.back() == "abortexposure");
    CHECK(rig.driver->get_camera_state() == CameraState::Idle);  // read back after the abort
}

TEST_CASE("Remote Alpaca Camera Driver - the setters are forwarded, written through and read back",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    // No asymmetric binning: the upstream sets BinY with BinX, and the read-back after the PUT shows it.
    c.set_bin_x(2);
    CHECK(c.get_bin_x() == 2);
    CHECK(c.get_bin_y() == 2);
    // The gain: written through, and ElectronsPerADU, which follows it, read again.
    CHECK(c.get_electrons_per_adu() == 2.9);
    c.set_gain(50);
    CHECK(c.get_gain() == 50);
    CHECK(c.get_electrons_per_adu() == Catch::Approx(2.5));
    c.set_offset(7);
    CHECK(c.get_offset() == 7);
    c.set_readout_mode(1);
    CHECK(c.get_readout_mode() == 1);
    c.set_num_y(3);
    c.set_start_x(1);
    CHECK(c.get_num_y() == 3);
    CHECK(c.get_start_x() == 1);
    c.set_cooler_on(true);
    CHECK(c.get_cooler_on());
    c.set_set_ccd_temperature(-15.5);
    CHECK(c.get_set_ccd_temperature() == -15.5);
    // The upstream holds them: the poll does not ask, and a reconnect reads the same values.
    c.set_connected(false);
    rig.connect();
    CHECK(c.get_bin_y() == 2);
    CHECK(c.get_gain() == 50);
    CHECK(c.get_set_ccd_temperature() == -15.5);
    CHECK(c.get_cooler_on());
    // PulseGuide: forwarded, IsPulseGuiding written true at once, the end left to the poll.
    c.pulse_guide(2, 500);
    CHECK(c.get_is_pulse_guiding());
    rig.upstream.finish_pulse();
    rig.poll();
    CHECK_FALSE(c.get_is_pulse_guiding());
    // A write the upstream refuses while it exposes passes through and writes nothing. (Back to a frame that fits the
    // sensor at bin 1 first: StartExposure checks it.)
    c.set_bin_x(1);
    c.set_start_x(0);
    c.start_exposure(10.0, true);
    CHECK(alpaca_error_message([&] { c.set_gain(60); }, AlpacaError::InvalidOperation) ==
          kPrefix + "The camera is exposing");
    CHECK(c.get_gain() == 50);
}

TEST_CASE("Remote Alpaca Camera Driver - Value range validation", "[remotealpaca][camera][unit]") {
    SECTION("an argument invalid whatever the camera is: InvalidValue before NotConnected, and sends nothing") {
        Rig rig;
        auto& c = *rig.driver;
        require_alpaca_error([&] { c.start_exposure(nan(), true); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { c.start_exposure(-1.0, true); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { c.set_bin_x(0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { c.set_num_y(-1); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { c.set_start_x(-1); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { c.set_set_ccd_temperature(nan()); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { c.set_readout_mode(-1); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { c.set_sub_exposure_duration(-1.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { c.pulse_guide(4, 100); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { c.pulse_guide(0, -1); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { c.pulse_guide(0, 60001); }, AlpacaError::InvalidValue);  // kDefaultMaxPulseGuideMs
        CHECK(rig.http.calls() == 0);
    }
    SECTION("a bound the Connect read: InvalidValue once connected, before any call") {
        Rig rig;
        rig.connect();
        auto& c = *rig.driver;
        const int puts = static_cast<int>(rig.puts().size());
        require_alpaca_error([&] { c.set_bin_x(3); }, AlpacaError::InvalidValue);                  // above MaxBinX 2
        require_alpaca_error([&] { c.set_readout_mode(2); }, AlpacaError::InvalidValue);           // two ReadoutModes
        require_alpaca_error([&] { c.start_exposure(7200.0, true); }, AlpacaError::InvalidValue);  // above 3600 s
        CHECK(static_cast<int>(rig.puts().size()) == puts);
    }
    SECTION("the frame and the gain are the upstream's to judge: its InvalidValue passes through unchanged") {
        Rig rig;
        rig.connect();
        auto& c = *rig.driver;
        c.set_num_x(9);  // the setter takes it; StartExposure refuses it (ConformU's Reject Bad XSize)
        CHECK(alpaca_error_message([&] { c.start_exposure(1.0, true); }, AlpacaError::InvalidValue) ==
              kPrefix + "The frame is off the sensor");
        CHECK(c.get_camera_state() == CameraState::Idle);  // a refused start writes nothing through
        rig.upstream.set_put_error("gain", 0x401, "Gain 500 is outside 0 to 100");
        CHECK(alpaca_error_message([&] { c.set_gain(500); }, AlpacaError::InvalidValue) ==
              kPrefix + "Gain 500 is outside 0 to 100");
        CHECK(c.get_gain() == 10);
    }
    SECTION("the factory refuses each config field out of range before any network call") {
        alpacacore::test::FakeTaskClock clock;
        FakeHttpClient http;
        for (const auto bad : {std::chrono::seconds(0), std::chrono::seconds(61)}) {
            require_alpaca_error(
                [&] { (void)remote::create_remotealpaca_camera(0, Rig::config(bad), http.client(), {}, clock); },
                AlpacaError::InvalidValue);
        }
        remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
        config.port = 0;
        require_alpaca_error([&] { (void)remote::create_remotealpaca_camera(0, config, http.client(), {}, clock); },
                             AlpacaError::InvalidValue);
        CHECK(http.calls() == 0);
    }
}

TEST_CASE("Remote Alpaca Camera Driver - Connect and Disconnect are forwarded to the upstream",
          "[remotealpaca][camera][unit]") {
    SECTION("Platform 7 upstream (ICameraV4): Connect and Disconnect") {
        Rig rig;
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 1);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 1);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
    SECTION("an ICameraV3 upstream has no Connect: Connected, and it keeps Offset") {
        Rig rig;
        rig.upstream.set("interfaceversion", "3");
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 0);
        CHECK(rig.upstream.upstream_connected());
        CHECK(rig.driver->get_offset() == 5);
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 0);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
}

TEST_CASE("Remote Alpaca Camera Driver - State machine contracts", "[remotealpaca][camera][unit]") {
    Rig rig;
    auto& c = *rig.driver;
    CHECK_FALSE(c.get_connected());
    CHECK(c.get_device_state().empty());
    require_alpaca_error([&] { (void)c.get_camera_state(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)c.get_image_array(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { c.start_exposure(1.0, true); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { c.abort_exposure(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { c.set_gain(1); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);

    rig.connect();
    CHECK(c.get_camera_state() == CameraState::Idle);  // idle after Connect
    CHECK_FALSE(c.get_image_ready());
    rig.driver->set_connected(true);  // idempotent: no second forward
    CHECK(rig.upstream.calls("connect") == 1);

    // A refused StartExposure is a lost link at once (decision 0009 point 2(a)): NotConnected, and AbortExposure too.
    rig.http.refuse_next(1);
    require_alpaca_error([&] { c.start_exposure(1.0, true); }, AlpacaError::NotConnected);
    CHECK_FALSE(c.get_connected());
    CHECK(c.get_device_state().empty());
    require_alpaca_error([&] { c.abort_exposure(); }, AlpacaError::NotConnected);
    CHECK(c.get_link_fault().find("connection refused") != std::string::npos);
    // Nothing relinks by itself (decision 0009 rule 4): the poll ended.
    const int calls = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(60));
    CHECK(rig.http.calls() == calls);
    // Only the client's Connect relinks.
    rig.connect();
    CHECK(c.get_link_fault().empty());
    CHECK(c.get_camera_state() == CameraState::Idle);

    // A refused image download loses the link too.
    rig.http.refuse_next(1);
    require_alpaca_error([&] { (void)c.get_image_array(); }, AlpacaError::NotConnected);
    CHECK_FALSE(c.get_connected());
    rig.connect();

    // Disconnected by the client: NotConnected again, and the poll ended.
    c.set_connected(false);
    require_alpaca_error([&] { (void)c.get_ccd_temperature(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)c.get_camera_x_size(); }, AlpacaError::NotConnected);
    CHECK(c.get_can_abort_exposure());  // the last Connect's value
    const int after = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(10));
    CHECK(rig.http.calls() == after);
}

TEST_CASE("Remote Alpaca Camera Driver - members an older upstream lacks are not implemented and never called",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    // An ICameraV1 upstream has none of the ICameraV2 and V3 members.
    rig.upstream.set("interfaceversion", "1");
    rig.connect();  // the missing members are never asked: the Connect stands
    auto& c = *rig.driver;
    const std::string lacks = "Upstream 127.0.0.1:11111 device 0 is an ICameraV1 and has no ";
    CHECK(alpaca_error_message([&] { (void)c.get_gain(); }, AlpacaError::NotImplemented) == lacks + "Gain");
    CHECK(alpaca_error_message([&] { (void)c.get_exposure_max(); }, AlpacaError::NotImplemented) ==
          lacks + "ExposureMax");
    CHECK(alpaca_error_message([&] { (void)c.get_offset(); }, AlpacaError::NotImplemented) == lacks + "Offset");
    CHECK(alpaca_error_message([&] { (void)c.get_sensor_name(); }, AlpacaError::NotImplemented) ==
          lacks + "SensorName");
    CHECK(alpaca_error_message([&] { c.set_gain(5); }, AlpacaError::NotImplemented) == lacks + "Gain");
    CHECK(alpaca_error_message([&] { c.set_offset(5); }, AlpacaError::NotImplemented) == lacks + "Offset");
    CHECK(alpaca_error_message([&] { c.set_readout_mode(0); }, AlpacaError::NotImplemented) == lacks + "ReadoutMode");
    CHECK(alpaca_error_message([&] { c.set_fast_readout(true); }, AlpacaError::NotImplemented) ==
          lacks + "FastReadout");
    CHECK_FALSE(c.get_can_fast_readout());
    // With no ExposureMin, StartExposure goes to the upstream as asked.
    c.start_exposure(0.0, true);
    CHECK(rig.upstream.last_start() == "0 true");
    rig.poll();
    CHECK(alpaca_error_message([&] { (void)c.get_percent_completed(); }, AlpacaError::NotImplemented) ==
          lacks + "PercentCompleted");
    for (const char* member : {"gain", "gainmax", "offset", "exposuremax", "sensorname", "readoutmodes",
                               "percentcompleted", "subexposureduration", "fastreadout"}) {
        INFO(member);
        CHECK(rig.upstream.calls(member) == 0);
    }
    CHECK(c.get_link_fault().empty());
    // A member the upstream itself lacks answers its own NotImplemented, unchanged (decision 0023 rule 5).
    CHECK(alpaca_error_message([&] { (void)c.get_heat_sink_temperature(); }, AlpacaError::NotImplemented) ==
          kPrefix + "HeatSinkTemperature is not implemented");
    // ImageArrayVariant is COM only; actions and Command* are not forwarded.
    require_alpaca_error([&] { (void)c.get_image_array_variant(); }, AlpacaError::NotImplemented);
    CHECK(c.get_supported_actions().empty());
    CHECK_FALSE(c.can_action("anything"));
    require_alpaca_error([&] { (void)c.action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)c.command_blind("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)c.command_bool("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)c.command_string("x", false); }, AlpacaError::NotImplemented);
    CHECK(rig.upstream.calls("action") == 0);
}

TEST_CASE("Remote Alpaca Camera Driver - an upstream error passes through unchanged with its host",
          "[remotealpaca][camera][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    rig.upstream.set_put_error("startexposure", 0x40B, "the shutter is jammed");
    CHECK(alpaca_error_message([&] { c.start_exposure(1.0, true); }, AlpacaError::InvalidOperation) ==
          kPrefix + "the shutter is jammed");
    // A refused start wrote nothing through, and an answered error is not a transport failure: the link stays up.
    CHECK(c.get_camera_state() == CameraState::Idle);
    CHECK(c.get_connected());
    CHECK(c.get_link_fault().empty());
    // An upstream error on a polled member is kept in the frame and answered by its getter.
    rig.upstream.set_error("ccdtemperature", 0x500, "the sensor thermistor is open");
    rig.poll();
    CHECK(alpaca_error_message([&] { (void)c.get_ccd_temperature(); }, 0x500) ==
          kPrefix + "the sensor thermistor is open");
    CHECK(c.get_link_fault().empty());
}

TEST_CASE("Remote Alpaca Camera Driver - Connect refuses an upstream that is this server",
          "[remotealpaca][camera][unit]") {
    // This server listens on every address (a wildcard bind) on the upstream's port: 127.0.0.1 is one of its own.
    Rig rig([] { return std::vector<remote::ServerAddress>{{"0.0.0.0", kUpstreamPort}}; });
    const std::string text =
        alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK(text.find("is this server") != std::string::npos);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.http.calls() == 0);
}

TEST_CASE("Remote Alpaca Camera Driver - the image download's cap and timeout follow the sensor",
          "[remotealpaca][camera][unit]") {
    // A 6248 x 4176 monochrome sensor (an IMX455): four bytes an element and 64 KiB over, and at 1 MiB/s that body
    // takes about 100 s, so the timeout is 5 s plus that.
    const auto mono = remote::image_fetch_bounds(6248, 4176, 1);
    CHECK(mono.max_body_bytes == std::size_t{6248} * 4176 * 4 + 65536);
    CHECK(mono.timeout == std::chrono::milliseconds(5000) +
                              std::chrono::milliseconds(mono.max_body_bytes * 1000 / remote::kImageTransferFloor));
    // A Color sensor's three planes: three times the elements.
    CHECK(remote::image_fetch_bounds(6248, 4176, 3).max_body_bytes == std::size_t{6248} * 4176 * 12 + 65536);
    // A small sensor still takes a JSON error reply (1 MiB), and its timeout is past the 5 s per-call one.
    const auto small = remote::image_fetch_bounds(8, 6, 1);
    CHECK(small.max_body_bytes == alpacacore::util::kHttpMaxResponseBytes);
    CHECK(small.timeout > std::chrono::milliseconds(5000));
    // A sensor size the upstream did not answer, or one larger than any real sensor, takes the ceilings.
    for (const auto& bounds : {remote::image_fetch_bounds(0, 0, 1), remote::image_fetch_bounds(1 << 20, 1 << 20, 3)}) {
        CHECK(bounds.max_body_bytes == remote::kImageBodyCeiling);
        CHECK(bounds.timeout == remote::kImageFetchTimeoutCeiling);
    }
}

// The client's ImageBytes decode (remote_alpaca_client.h): rank 3 in the wire's x-major order, every integer type it
// reads, and each malformed reply a transport error, never an image.
TEST_CASE("Remote Alpaca client - get_image_array decodes ImageBytes and refuses a malformed reply",
          "[remotealpaca][client][unit]") {
    FakeHttpClient http;
    std::string body;
    std::string content_type = "application/imagebytes";
    http.set_responder(
        [&](const FakeHttpClient::Request&) { return alpacacore::util::HttpResponse{200, body, content_type}; });
    remote::RemoteAlpacaClient client(http.client(), {"127.0.0.1", kUpstreamPort, "camera", 0}, 7);
    const auto get = [&] { return client.get_image_array(std::size_t{1} << 20, std::chrono::seconds(10)); };

    // Rank 3, 2 x 1 pixels, Int32: the wire is x, then y, then the plane.
    std::string data;
    for (const std::int32_t value : {1, 2, 3, -4, 5, 6}) {
        for (int shift = 0; shift < 32; shift += 8)
            data += static_cast<char>((static_cast<std::uint32_t>(value) >> shift) & 0xFF);
    }
    body = image_bytes(0, 2, 3, 2, 1, 3, data);
    const auto color = get();
    CHECK(color.rank == 3);
    CHECK(color.width == 2);
    CHECK(color.height == 1);
    CHECK(color.data == std::vector<std::int32_t>{1, 2, 3, -4, 5, 6});
    // Int16 keeps its sign, Byte and UInt16 are unsigned.
    body = image_bytes(0, 1, 2, 1, 1, 0, std::string("\xFE\xFF", 2));
    CHECK(get().data == std::vector<std::int32_t>{-2});
    body = image_bytes(0, 8, 2, 1, 1, 0, std::string("\xFE\xFF", 2));
    CHECK(get().data == std::vector<std::int32_t>{65534});
    body = image_bytes(0, 6, 2, 1, 1, 0, std::string("\xC8", 1));
    CHECK(get().data == std::vector<std::int32_t>{200});

    const auto refused = [&](const char* why) {
        INFO(why);
        return alpaca_error_message([&] { (void)get(); }, AlpacaError::DriverException);
    };
    body = image_bytes(0, 8, 2, 2, 2, 0, std::string(7, '\0'));  // 4 UInt16 need 8 bytes
    CHECK(refused("short data").find("is shorter than its shape") != std::string::npos);
    body = image_bytes(0, 3, 2, 1, 1, 0, std::string(8, '\0'));  // Double
    CHECK(refused("double").find("TransmissionElementType 3") != std::string::npos);
    body = image_bytes(0, 9, 2, 1, 1, 0, std::string("\xFF\xFF\xFF\xFF", 4));  // UInt32 above INT32_MAX
    CHECK(refused("uint32").find("above the Int32 range") != std::string::npos);
    body = image_bytes(0, 2, 3, 1, 1, 4, std::string(16, '\0'));  // four planes
    CHECK(refused("planes").find("shape") != std::string::npos);
    body = image_bytes(0, 2, 2, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, std::string(4, '\0'));  // a shape that would overflow
    CHECK(refused("overflow").find("shape") != std::string::npos);
    body = std::string(10, '\0');
    CHECK(refused("metadata").find("44-byte metadata") != std::string::npos);
    // An ImageBytes error: the number unchanged, the text from DataStart with the host in front.
    body = image_bytes(0x40B, 0, 0, 0, 0, 0, "not ready");
    CHECK(alpaca_error_message([&] { (void)get(); }, AlpacaError::InvalidOperation) == kPrefix + "not ready");
    // The request asked for ImageBytes, with the cap and the timeout given.
    const auto requests = http.requests();
    REQUIRE_FALSE(requests.empty());
    CHECK(requests.back().accept == "application/imagebytes");
    CHECK(requests.back().max_body_bytes == std::size_t{1} << 20);
    CHECK(requests.back().timeout == std::chrono::seconds(10));
}

TEST_CASE("Remote Alpaca client - get_utc_time reads the ASCOM UTC form and refuses any other",
          "[remotealpaca][client][unit]") {
    FakeHttpClient http;
    std::string value;
    http.set_responder([&](const FakeHttpClient::Request&) {
        return alpacacore::util::HttpResponse{
            200,
            "{\"Value\":\"" + value + "\",\"ClientTransactionID\":1,\"ServerTransactionID\":1,\"ErrorNumber\":0}",
            {}};
    });
    remote::RemoteAlpacaClient client(http.client(), {"127.0.0.1", kUpstreamPort, "camera", 0}, 7);
    value = "2026-11-26T12:00:00";
    const auto noon = client.get_utc_time("lastexposurestarttime");
    CHECK(noon == alpacacore::test::parse_timestamp("2026-11-26T12:00:00.000Z"));
    value = "2026-11-26T12:00:00.25Z";
    CHECK(client.get_utc_time("lastexposurestarttime") - noon == std::chrono::milliseconds(250));
    for (const char* bad : {"2026-11-26 12:00:00", "2026-13-26T12:00:00", "2026-02-30T12:00:00", "2026-11-26T12:00:00.",
                            "2026-11-26T12:00:00+01:00", "yesterday"}) {
        INFO(bad);
        value = bad;
        require_alpaca_error([&] { (void)client.get_utc_time("lastexposurestarttime"); }, AlpacaError::DriverException);
    }
}

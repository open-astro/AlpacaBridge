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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "camera_percent_cooler_checks.h"
#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_task_clock.h"
#include "fake_zwo_sdk.h"
#include "getter_stall_probe.h"

namespace {

using alpacacore::test::require_alpaca_error;

} // namespace

TEST_CASE("ZWO Camera Driver - Defaults", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // Name is "ZWO Camera" when no camera is plugged in, or the SDK name when detected
    CHECK(driver->get_name().find("ZWO") != std::string::npos);
}

TEST_CASE("ZWO Camera Driver - Device metadata", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(3, 1);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ZWO ASI Camera Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO Camera Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);  // ICameraV4 (Platform 7)
    // Not derived from the device number or the enumeration index (#914).
    CHECK(driver->get_unique_id().rfind("ZWO_UID_", 0) == 0);
    CHECK(driver->get_unique_id() == driver->get_unique_id());
}

TEST_CASE("ZWO Camera Driver - UniqueID follows the config entry, not the device number", "[zwo][camera][unit]") {
    alpacacore::vendor::zwo::ZwoCameraBinding serial_less;
    serial_less.identity.camera_name = "ZWO ASI120MM Mini";
    serial_less.unique_id = "ZWO_UID_00112233445566ff";
    alpacacore::vendor::zwo::ZwoCameraBinding with_serial;
    with_serial.identity.serial = "0c190e111d020900";
    with_serial.unique_id = "ZWO_UID_ignored";

    // Same stored value on two starts and under two device numbers.
    CHECK(alpacacore::vendor::zwo::create_zwo_camera_bound(0, serial_less)->get_unique_id() ==
          "ZWO_UID_00112233445566ff");
    CHECK(alpacacore::vendor::zwo::create_zwo_camera_bound(1, serial_less)->get_unique_id() ==
          "ZWO_UID_00112233445566ff");
    CHECK(alpacacore::vendor::zwo::create_zwo_camera_bound(1, with_serial)->get_unique_id() ==
          "ZWO_SN_0c190e111d020900");
}

TEST_CASE("ZWO Camera Driver - DeviceState is empty when disconnected", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    // A disconnected driver's DeviceState is the empty list -- no TimeStamp
    // (ASCOM read-all FAQ), and none of the getters that answer a default
    // (CameraState, PercentCompleted) leak into it.
    CHECK(driver->get_device_state().empty());
}

TEST_CASE("ZWO Camera Driver - Not connected throws", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    CHECK_THROWS_AS(driver->get_ccd_temperature(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_gain(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_gain(100), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_offset(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->start_exposure(1.0, true), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->stop_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->abort_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->pulse_guide(0, 100), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_image_array(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_image_ready(), alpacacore::AlpacaException);
}

TEST_CASE("ZWO Camera Driver - Disconnected state", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_is_pulse_guiding() == false);
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == true);
    CHECK(driver->get_can_asymmetric_bin() == false);
}

TEST_CASE("ZWO Camera Driver - Unsupported actions", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("anything", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("", false), alpacacore::AlpacaException);
}

TEST_CASE("ZWO Camera Driver - Sub-exposure not supported", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    CHECK_THROWS_AS(driver->get_sub_exposure_duration(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_sub_exposure_duration(1.0), alpacacore::AlpacaException);
}

TEST_CASE("ZWO Camera Driver - ASCOM Error Codes", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    require_alpaca_error([&]() { driver->start_exposure(-1.0, true); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->start_exposure(std::numeric_limits<double>::quiet_NaN(), true); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_ccd_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_gain(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_gain(100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offset(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->start_exposure(1.0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->stop_exposure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->abort_exposure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->pulse_guide(0, 100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_array(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ZWO Camera Driver - State Machine Contracts", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == true);
}

namespace {

std::string connect_error(alpacacore::CameraDriver& driver) {
    try {
        driver.set_connected(true);
    } catch (const alpacacore::AlpacaException& ex) {
        return ex.what();
    }
    return "";
}

}  // namespace

// Issue #738: a camera registered by cameraId lost that id the first time a
// client read its name while it was disconnected, so every later connect
// failed with "Camera ID not specified". A name query must not change what a
// connect does. The id is 256, the SDK's ASICAMERA_ID_MAX, which no attached
// camera can have, so the connect fails with or without cameras on the bus;
// the point is that both drivers fail the same way, on the configured id.
TEST_CASE("ZWO Camera Driver - A name query keeps the configured camera id", "[zwo][camera][unit]") {
    constexpr int kNoSuchCameraId = 256;
    auto untouched = alpacacore::vendor::zwo::create_zwo_camera(0, kNoSuchCameraId);
    const std::string expected = connect_error(*untouched);
    REQUIRE_FALSE(expected.empty());

    auto queried = alpacacore::vendor::zwo::create_zwo_camera(0, kNoSuchCameraId);
    (void)queried->get_name();
    const std::string actual = connect_error(*queried);

    CHECK(actual != "Camera ID not specified");
    CHECK(actual == expected);
}

// Issue #1477: ICameraV4 PercentCompleted raises InvalidOperation outside Exposing; the cooler members raise
// PropertyNotImplemented on a camera without a cooler.
TEST_CASE("ZWO Camera Driver - PercentCompleted only while Exposing", "[zwo][camera][unit]") {
    alpacacore::test::FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    auto camera = alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);
    // The retry window is measured from the epoch before the first retry.
    clock.advance(std::chrono::seconds(1));

    SECTION("idle before the first exposure") { alpacacore::test::require_percent_completed_unavailable(*camera); }
    SECTION("idle after a completed exposure") {
        camera->start_exposure(0.001, true);
        REQUIRE(camera->get_image_ready());
        alpacacore::test::require_percent_completed_unavailable(*camera);
    }
    SECTION("after a failed exposure") {
        // Three Failed statuses exhaust the retries (the driver waits 250 ms on the task clock between them);
        // the old value here was 100 because the failed frame still counted as an image.
        camera->start_exposure(0.001, true);
        sdk.script_exposure_status({alpacacore::vendor::zwo::ZWOExposureStatus::Failed,
                                    alpacacore::vendor::zwo::ZWOExposureStatus::Failed,
                                    alpacacore::vendor::zwo::ZWOExposureStatus::Failed});
        CHECK_FALSE(camera->get_image_ready());
        clock.advance(std::chrono::milliseconds(250));
        CHECK_FALSE(camera->get_image_ready());
        clock.advance(std::chrono::milliseconds(250));
        alpacacore::test::require_error_code([&] { (void)camera->get_image_ready(); },
                                             alpacacore::AlpacaError::DriverException);
        alpacacore::test::require_percent_completed_unavailable(*camera);
    }
    SECTION("while exposing") {
        sdk.auto_complete_exposure = false;
        camera->start_exposure(30.0, true);
        alpacacore::test::require_percent_completed_available(*camera);
    }
}

// ICameraV4: LastExposureDuration / LastExposureStartTime raise InvalidOperation until an image has been
// successfully acquired (issue #1477). A failed exposure never publishes them; an earlier success survives it.
TEST_CASE("ZWO Camera Driver - LastExposure values follow successful images only", "[zwo][camera][unit]") {
    alpacacore::test::FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    auto camera = alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);
    clock.advance(std::chrono::seconds(1));
    const auto fail_exposure = [&](double duration) {
        camera->start_exposure(duration, true);
        sdk.script_exposure_status({alpacacore::vendor::zwo::ZWOExposureStatus::Failed,
                                    alpacacore::vendor::zwo::ZWOExposureStatus::Failed,
                                    alpacacore::vendor::zwo::ZWOExposureStatus::Failed});
        CHECK_FALSE(camera->get_image_ready());
        clock.advance(std::chrono::milliseconds(250));
        CHECK_FALSE(camera->get_image_ready());
        clock.advance(std::chrono::milliseconds(250));
        alpacacore::test::require_error_code([&] { (void)camera->get_image_ready(); },
                                             alpacacore::AlpacaError::DriverException);
    };

    SECTION("before any exposure") { alpacacore::test::require_last_exposure_unset(*camera); }
    SECTION("a failed first exposure publishes nothing") {
        fail_exposure(0.001);
        alpacacore::test::require_last_exposure_unset(*camera);
    }
    SECTION("a later failure keeps the earlier success") {
        camera->start_exposure(0.001, true);
        REQUIRE(camera->get_image_ready());
        const auto start = camera->get_last_exposure_start_time();
        CHECK(camera->get_last_exposure_duration() == 0.001);
        fail_exposure(0.002);
        CHECK(camera->get_last_exposure_duration() == 0.001);
        CHECK(camera->get_last_exposure_start_time() == start);
    }
    SECTION("a new success replaces the earlier one") {
        camera->start_exposure(0.001, true);
        REQUIRE(camera->get_image_ready());
        camera->start_exposure(0.002, true);
        REQUIRE(camera->get_image_ready());
        CHECK(camera->get_last_exposure_duration() == 0.002);
    }
}

TEST_CASE("ZWO Camera Driver - cooler members need the cooler capability", "[zwo][camera][unit]") {
    alpacacore::test::FakeZWOSDK sdk;
    sdk.report_cooler_controls = false;
    auto camera = alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk);
    camera->set_connected(true);
    CHECK_FALSE(camera->get_can_get_cooler_power());
    alpacacore::test::require_no_cooler_members(*camera);
}

TEST_CASE("ZWO Camera Driver - cooler members work with the cooler capability", "[zwo][camera][unit]") {
    alpacacore::test::FakeZWOSDK sdk;
    auto camera = alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk);
    camera->set_connected(true);
    REQUIRE(camera->get_can_get_cooler_power());
    camera->set_cooler_on(true);
    CHECK(camera->get_cooler_on());
    CHECK(camera->get_cooler_power() >= 0.0);
}

// Issue #1277: the SDK's ElecPerADU is one static figure that does not follow gain, and ASCOM wants the value in
// the current mode, so a client gets PropertyNotImplemented, never a number that is wrong at every other gain. An
// unknown SensorName is the empty string (ICameraV4 SensorName), never a "ZWO Sensor" placeholder.
TEST_CASE("ZWO Camera Driver - ElectronsPerADU and FullWellCapacity are not implemented, SensorName has no placeholder",
          "[zwo][camera][unit]") {
    alpacacore::test::FakeZWOSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk);

    CHECK(driver->get_sensor_name().empty());
    require_alpaca_error([&] { (void)driver->get_electrons_per_adu(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_full_well_capacity(); }, alpacacore::AlpacaError::NotConnected);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_sensor_name() == "Fake ZWO Camera");
    for (const int gain : {0, 300}) {
        driver->set_gain(gain);
        require_alpaca_error([&] { (void)driver->get_electrons_per_adu(); },
                             alpacacore::AlpacaError::PropertyNotImplemented);
        require_alpaca_error([&] { (void)driver->get_full_well_capacity(); },
                             alpacacore::AlpacaError::PropertyNotImplemented);
    }
    for (const auto& s : driver->get_device_state()) {
        CHECK(s.name != "ElectronsPerADU");
        CHECK(s.name != "FullWellCapacity");
    }
    driver->set_connected(false);
}

// Decision 0016 rules 1 and 4: Name and the other identity getters need no device access, so a connect that
// holds the SDK open must not park them. The open is held on a latch; the getters run on the test thread.
// Falsified by: zwo_camera_driver.cpp set_connected(true) holding mutex_ across open_camera_link() (get_name() then
// waits until the latch is released and the timed call below fails its budget).
TEST_CASE("ZWO Camera Driver - Name and identity getters answer while a connect holds the SDK open",
          "[zwo][camera][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    std::atomic<bool> open_entered{false};
    std::atomic<bool> release_open{false};
    std::atomic<bool> info_read_during_open{false};
    sdk.before_call = [&](const std::string& call) {
        if (call == "get_camera_info_by_id" && open_entered.load() && !release_open.load()) {
            info_read_during_open = true;
        }
        if (call != "open_camera") return;
        open_entered = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release_open.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    auto camera = alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);

    std::thread connector([&] { camera->set_connected(true); });
    REQUIRE(wait_for_flag(open_entered));

    CHECK(timed([&] { (void)camera->get_name(); }) < kGetterBudget);
    CHECK(timed([&] { (void)camera->get_unique_id(); }) < kGetterBudget);
    CHECK(timed([&] { (void)camera->get_sensor_name(); }) < kGetterBudget);
    CHECK(timed([&] { CHECK_FALSE(camera->get_connected()); }) < kGetterBudget);
    CHECK_FALSE(info_read_during_open.load());  // the name getter does not reach the SDK behind the open
    CHECK_FALSE(release_open.load());           // the open is still held, so the timings above are not vacuous

    release_open = true;
    connector.join();
    CHECK(camera->get_connected());
    CHECK(camera->get_name() == sdk.camera.name);
    camera->set_connected(false);
}

// Decision 0016 rule 6: a name refresh that read the SDK by id while a connect ran must not overwrite what the
// connect published. The refresh is held inside get_camera_info_by_id on a latch, a connect completes, the SDK
// then reports another name, and the refresh is released.
// Falsified by: zwo_camera_driver.cpp refresh_cached_camera_info_if_needed() writing camera_info_ without the
// connected_/connect_in_flight_/camera_id_ re-check (get_name() then returns the stale refresh name).
TEST_CASE("ZWO Camera Driver - A name refresh that overlapped a connect does not overwrite the connect's info",
          "[zwo][camera][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    static thread_local bool is_refresher = false;
    std::atomic<bool> refresh_entered{false};
    std::atomic<bool> release_refresh{false};
    sdk.before_call = [&](const std::string& call) {
        if (call != "get_camera_info_by_id" || !is_refresher) return;
        refresh_entered = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release_refresh.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    auto camera = alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);

    std::thread refresher([&] {
        is_refresher = true;
        (void)camera->get_name();
    });
    REQUIRE(wait_for_flag(refresh_entered));

    const std::string connect_name = sdk.camera.name;
    camera->set_connected(true);
    sdk.camera.name = "Stale Refresh Name";
    release_refresh = true;
    refresher.join();

    CHECK(camera->get_name() == connect_name);
    camera->set_connected(false);
}

// ---- Camera replug and link loss (issue #1667, decision 0009) ----

namespace {

using alpacacore::AlpacaError::NotConnected;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::FakeZWOSDK;
using alpacacore::vendor::zwo::ZWOExposureStatus;

constexpr const char* kSerial = "FAKE-ZWO-17";

// Every WARN logged while it lives; restores the previous sink on any exit.
struct WarnCapture {
    std::vector<std::string> messages;
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    WarnCapture() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn) messages.emplace_back(message);
            });
    }
    ~WarnCapture() { alpacacore::logging::set_log_sink(previous); }
    WarnCapture(const WarnCapture&) = delete;
    WarnCapture& operator=(const WarnCapture&) = delete;
};

// A camera with an exposure in flight, the clock past the retry window.
struct ExposingCamera {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    std::unique_ptr<alpacacore::CameraDriver> camera =
        alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);

    ExposingCamera() {
        camera->set_connected(true);
        clock.advance(std::chrono::seconds(1));
        sdk.auto_complete_exposure = false;
        camera->start_exposure(30.0, true);
    }
};

// Only armed for poll_exposure_status's unlocked call, never a loss under mutex_ or the trigger lock.
struct LossPublicationGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool arrived{false};
    bool released{false};
    bool timed_out{false};

    void pause() noexcept {
        std::unique_lock lock(mutex);
        arrived = true;
        cv.notify_all();
        timed_out = !cv.wait_for(lock, std::chrono::seconds(2), [&] { return released; });
    }
    bool wait_for_arrival() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(2), [&] { return arrived; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};

void check_loss_publication_after_transition(bool reconnect) {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    auto gate = std::make_shared<LossPublicationGate>();
    auto camera =
        alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk, clock, {[gate] { gate->pause(); }});
    camera->set_connected(true);
    sdk.auto_complete_exposure = false;
    camera->start_exposure(30.0, true);

    // A peer observes removal first, retiring the shared open. The camera's next status call
    // therefore throws NotConnected before its raw SDK body, without entering the retry burst.
    const int id = sdk.camera.camera_id;
    const auto peer = sdk.open_camera_link(id);
    sdk.removed = true;
    require_alpaca_error(
        [&] {
            sdk.with_link(id, peer.link, [&] {
                long value = 0;
                bool automatic = false;
                (void)sdk.get_control_value(id, FakeZWOSDK::ControlType::Gain, value, automatic);
            });
        },
        NotConnected);
    sdk.close_camera_link(id, peer.link);

    std::packaged_task<void()> report([&] {
        try {
            (void)camera->get_image_ready();
        } catch (const alpacacore::AlpacaException&) {
            // The racing lifecycle may clear the pending exposure; only LinkFault/state are asserted here.
        }
    });
    auto report_done = report.get_future();
    std::thread reporter(std::move(report));
    struct ReleaseAndJoin {
        LossPublicationGate& gate;
        std::thread& reporter;
        ~ReleaseAndJoin() {
            gate.release();
            reporter.join();
        }
    } cleanup{*gate, reporter};
    REQUIRE(gate->wait_for_arrival());
    CHECK_FALSE(camera->get_connected());

    std::packaged_task<void()> transition([&] {
        camera->set_connected(false);  // idempotent: the loss has already closed the epoch
        if (reconnect) {
            sdk.replug({{23, kSerial}});
            camera->set_connected(true);
        }
    });
    auto transition_done = transition.get_future();
    std::thread lifecycle(std::move(transition));
    const bool completed = transition_done.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!completed) gate->release();
    lifecycle.join();
    REQUIRE(completed);
    transition_done.get();
    CHECK(camera->get_connected() == reconnect);
    CHECK(camera->get_link_fault().empty());

    gate->release();
    REQUIRE(report_done.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    report_done.get();
    CHECK_FALSE(gate->timed_out);
    CHECK(camera->get_connected() == reconnect);
    CHECK(camera->get_link_fault().empty());
    camera->set_connected(false);  // also audit the ordinary Disconnect branch after a successful reconnect
    CHECK_FALSE(camera->get_connected());
    CHECK(camera->get_link_fault().empty());
}

}  // namespace

TEST_CASE("ZWO Camera Driver - a delayed loss reporter cannot restore LinkFault after reconnect",
          "[zwo][camera][unit][link][loss-publication]") {
    check_loss_publication_after_transition(true);
}

TEST_CASE("ZWO Camera Driver - a delayed loss reporter cannot restore LinkFault after idempotent Disconnect",
          "[zwo][camera][unit][link][loss-publication]") {
    check_loss_publication_after_transition(false);
}

// Falsified by: zwo_camera_driver.cpp poll_exposure_status() running the retry burst after the stop met a removal
// error (the exposure fails with 1280 and Connected stays true, as on the rig); on_link() not calling note_loss().
// Decision 0009 rule 2a: the first removal error drops the link, and no retry re-triggers the exposure.
TEST_CASE("ZWO Camera Driver - a pulled cable during an exposure loses the link at once", "[zwo][camera][unit][link]") {
    WarnCapture log;
    ExposingCamera rig;
    const int starts = rig.sdk.call_count("start_exposure");
    const int writes = rig.sdk.call_count("set_control_value");
    rig.sdk.removed = true;  // the status read answers Failed, the retry's stop ASI_ERROR_CAMERA_REMOVED

    require_alpaca_error([&] { (void)rig.camera->get_image_ready(); }, NotConnected);

    CHECK_FALSE(rig.camera->get_connected());
    CHECK(rig.sdk.call_count("start_exposure") == starts);  // no retry re-triggered the exposure
    CHECK(rig.sdk.call_count("set_control_value") == writes);
    require_alpaca_error([&] { (void)rig.camera->get_camera_state(); }, NotConnected);
    require_alpaca_error([&] { (void)rig.camera->get_ccd_temperature(); }, NotConnected);
    CHECK(rig.camera->get_device_state().empty());
    std::vector<std::string> lost;
    for (const auto& message : log.messages) {
        if (message.find(" lost (") != std::string::npos) lost.push_back(message);
    }
    REQUIRE(lost.size() == 1);  // one WARN, with the serial
    CHECK(lost.front().find(kSerial) != std::string::npos);
}

// Falsified by: zwo_camera_driver.cpp on_link() not calling note_loss() (Connected stays true). The wrapper side,
// zwo_sdk_wrapper.cpp get_control_value() answering false on a removal code (which hid a pulled cable from every
// temperature read): MUTATION: not runnable here: it needs the real vendor library and a camera, which these tests
// replace with the fake SDK (rig check: TODO(#1667)).
TEST_CASE("ZWO Camera Driver - a removal error on a temperature read loses the link", "[zwo][camera][unit][link]") {
    FakeZWOSDK sdk;
    auto camera = alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk);
    camera->set_connected(true);
    CHECK(camera->get_ccd_temperature() == Catch::Approx(15.0));
    sdk.removed = true;

    require_alpaca_error([&] { (void)camera->get_ccd_temperature(); }, NotConnected);

    CHECK_FALSE(camera->get_connected());
    require_alpaca_error([&] { camera->set_bin_x(1); }, NotConnected);
    CHECK(camera->get_link_fault().find("was removed from USB") != std::string::npos);
    camera->set_connected(false);  // decision 0009 rule 5: a Disconnect clears the loss text
    CHECK(camera->get_link_fault().empty());
}

// Falsified by: zwo_camera_driver.cpp set_connected(false) treating a lost link as still connected (the Disconnect
// never completes), or the next Connect binding the old id instead of the serial read at the first connect.
TEST_CASE("ZWO Camera Driver - Disconnect after a loss completes and Connect reopens by serial on the new id",
          "[zwo][camera][unit][link]") {
    ExposingCamera rig;
    const int old_id = rig.sdk.camera.camera_id;
    rig.sdk.removed = true;
    require_alpaca_error([&] { (void)rig.camera->get_image_ready(); }, NotConnected);

    rig.camera->set_connected(false);
    CHECK_FALSE(rig.camera->get_connected());

    rig.sdk.replug({{23, kSerial}});
    rig.camera->set_connected(true);
    CHECK(rig.camera->get_connected());
    CHECK(rig.sdk.holders(23) == 1);
    CHECK(rig.sdk.holders(old_id) == 0);
    rig.sdk.auto_complete_exposure = true;
    rig.camera->start_exposure(0.001, true);
    CHECK(rig.camera->get_image_ready());
    CHECK(rig.sdk.open_count() == 1);

    rig.camera->set_connected(false);
    CHECK(rig.sdk.open_count() == 0);
}

// Falsified by: zwo_camera_driver.cpp resolve_camera_id() binding the configured id again after the first connect
// (cameras whose ids swapped while disconnected are mixed up).
TEST_CASE("ZWO Camera Driver - two cameras swapped while disconnected are told apart by serial",
          "[zwo][camera][unit][link]") {
    FakeZWOSDK sdk;
    sdk.set_units({{17, "SERIAL-A"}, {18, "SERIAL-B"}});
    auto camera = alpacacore::vendor::zwo::create_zwo_camera(0, 17, sdk);
    camera->set_connected(true);
    CHECK(sdk.holders(17) == 1);
    CHECK(camera->get_unique_id() == "ZWO_SN_SERIAL-A");
    camera->set_connected(false);

    sdk.set_units({{17, "SERIAL-B"}, {18, "SERIAL-A"}});
    camera->set_connected(true);

    CHECK(sdk.holders(18) == 1);  // A moved to id 18
    CHECK(sdk.holders(17) == 0);  // and B at 17 stays closed
    CHECK(camera->get_unique_id() == "ZWO_SN_SERIAL-A");
    camera->set_connected(false);
}

// Falsified by: zwo_camera_driver.cpp set_connected() taking exposure_trigger_mutex_ before it publishes the closed
// state (a retry burst stuck in a hung SDK call keeps Connected true through every Disconnect, as on the rig).
TEST_CASE("ZWO Camera Driver - Disconnect reads false while a retry burst is stuck in the SDK",
          "[zwo][camera][unit][link]") {
    ExposingCamera rig;
    std::atomic<bool> stuck{false};
    std::atomic<bool> release{false};
    static thread_local bool is_poller = false;
    rig.sdk.before_call = [&](const std::string& call) {
        if (call != "stop_exposure" || !is_poller) return;
        stuck = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    rig.sdk.set_exposure_status(ZWOExposureStatus::Failed);  // the retry burst starts at the next poll
    std::thread poller([&] {
        is_poller = true;
        try {
            (void)rig.camera->get_image_ready();
        } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch): any outcome after Disconnect
        }
    });
    REQUIRE(alpacacore::test::wait_for_flag(stuck));

    std::thread disconnect([&] { rig.camera->set_connected(false); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (rig.camera->get_connected() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool connected_while_stuck = rig.camera->get_connected();
    release = true;
    poller.join();
    disconnect.join();

    CHECK_FALSE(connected_while_stuck);
    CHECK_FALSE(rig.camera->get_connected());
    CHECK(rig.sdk.open_count() == 0);
}

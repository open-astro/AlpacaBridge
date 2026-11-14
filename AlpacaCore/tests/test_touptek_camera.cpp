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
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_task_clock.h"
#include "fake_touptek_sdk.h"

namespace {

using alpacacore::test::require_alpaca_error;

} // namespace

TEST_CASE("ToupTek Camera Driver - Defaults", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // Name is "ToupTek Camera" when no camera is plugged in, or the SDK displayname
    // (model code, may not contain "ToupTek") when one is detected. Either way it's non-empty.
    CHECK_FALSE(driver->get_name().empty());
}

TEST_CASE("ToupTek Camera Driver - Device metadata", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(3, 1);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ToupTek Camera Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ToupTek Camera Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);  // ICameraV4 (Platform 7)
    // Without a connected camera the serial number is unknown, so the unique id falls
    // back to the device number.
    CHECK(driver->get_unique_id() == "TOUPTEK_3");
}

TEST_CASE("ToupTek Camera Driver - Not connected throws", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    require_alpaca_error([&] { (void)driver->get_ccd_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_gain(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_gain(100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_offset(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek Camera Driver - Disconnected state", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_is_pulse_guiding() == false);
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == true);
    CHECK(driver->get_can_asymmetric_bin() == false);
    CHECK(driver->get_has_shutter() == false);
}

TEST_CASE("ToupTek Camera Driver - Unsupported actions", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&] { (void)driver->action("anything", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)driver->command_blind("", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->command_bool("", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->command_string("", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ToupTek Camera Driver - Sub-exposure not supported", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    require_alpaca_error([&] { (void)driver->get_sub_exposure_duration(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_sub_exposure_duration(1.0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek Camera Driver - ASCOM Error Codes", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    // TODO: ToupTek throws NotImplemented (0x400) for several properties when no
    // camera handle exists instead of NotConnected (0x407). Fix the driver so all
    // disconnected operations return NotConnected for ConformU compliance.
    auto check_throws_not_connected_or_not_implemented = [&](const std::function<void()>& fn) {
        try {
            fn();
            FAIL("Expected AlpacaException");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK((ex.error_code() == alpacacore::AlpacaError::NotConnected ||
                   ex.error_code() == alpacacore::AlpacaError::NotImplemented));
        }
    };

    check_throws_not_connected_or_not_implemented([&]() { driver->get_ccd_temperature(); });
    check_throws_not_connected_or_not_implemented([&]() { driver->get_gain(); });
    check_throws_not_connected_or_not_implemented([&]() { driver->set_gain(100); });
    check_throws_not_connected_or_not_implemented([&]() { driver->get_offset(); });
    require_alpaca_error([&]() { driver->start_exposure(1.0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->stop_exposure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->abort_exposure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->pulse_guide(0, 100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_array(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek Camera Driver - State Machine Contracts", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == true);
}

TEST_CASE("ToupTek Camera Driver - Readout modes (conversion gain + High Full Well)", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    // ReadoutModes fold the camera's conversion-gain (HCG/LCG/HDR) and High Full
    // Well capabilities into one flat list. Disconnected, this driver is built
    // on the real SDK, so the contents depend on the attached camera and only
    // hardware-independent invariants are asserted: the LIST is readable (never
    // empty, at least "Normal"), matching every other camera driver: imaging
    // clients enumerate ReadoutModes before connecting. The current-index getter
    // needs live registers, so it throws NotConnected; `set_readout_mode`
    // validates the range BEFORE the connection check, so an out-of-range index
    // is InvalidValue even disconnected. The connected half below runs over
    // FakeToupTekSDK and asserts the exact list and the mode toggles.
    REQUIRE_FALSE(driver->get_readout_modes().empty());
    require_alpaca_error([&]() { driver->get_readout_mode(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_readout_mode(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_readout_mode(999999); }, alpacacore::AlpacaError::InvalidValue);

    // Connected over the fake: the exact list and the register each mode writes.
    alpacacore::test::FakeToupTekSDK fake;
    auto info = alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One");
    info.supports_cg = true;
    info.supports_high_fullwell = true;
    fake.cameras.push_back(info);
    auto connected = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    connected->set_connected(true);
    CHECK(connected->get_readout_modes() == std::vector<std::string>{"HCG", "LCG", "High Full Well"});
    connected->set_readout_mode(2);
    CHECK(fake.call_count("put_high_fullwell") == 1);
    CHECK(connected->get_readout_mode() == 2);
    connected->set_readout_mode(0);
    CHECK(connected->get_readout_mode() == 0);
    connected->set_connected(false);

    // A camera with HDR conversion gain adds it after High Full Well.
    fake.cameras[0].supports_cghdr = true;
    auto hdr = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    hdr->set_connected(true);
    CHECK(hdr->get_readout_modes() == std::vector<std::string>{"HCG", "LCG", "High Full Well", "HDR"});
    hdr->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - Offset (black level)", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    // Offset maps to the black level on cameras reporting TOUPCAM_FLAG_BLACKLEVEL.
    // Operational reads require a connection first, so disconnected they report
    // NotConnected (not a generic DriverException); the connected value range is
    // checked below over FakeToupTekSDK. Named offset descriptions are never implemented
    // (integer OffsetMin/OffsetMax mode), regardless of hardware; disconnected, NotConnected comes first (#1919).
    require_alpaca_error([&]() { driver->get_offset(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_offset(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offset_min(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offset_max(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offsets(); }, alpacacore::AlpacaError::NotConnected);

    // Connected over the fake: the value round-trips through the black-level
    // register and out-of-range values are rejected.
    alpacacore::test::FakeToupTekSDK fake;
    auto info = alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One");
    info.supports_blacklevel = true;
    fake.cameras.push_back(info);
    auto connected = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    connected->set_connected(true);
    connected->set_offset(7);
    CHECK(fake.call_count("put_blacklevel") == 1);
    CHECK(connected->get_offset() == 7);
    CHECK(connected->get_offset_min() == 0);
    require_alpaca_error([&]() { connected->set_offset(connected->get_offset_max() + 1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { connected->set_offset(-1); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(connected->get_offset() == 7);
    connected->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - AbortExposure during the stream restart is not undone",
          "[touptek][camera][unit][cc16]") {
    using Clock = std::chrono::steady_clock;
    std::atomic<bool> armed{false};
    std::atomic<bool> in_restart{false};
    std::atomic<bool> abort_stop{false};
    alpacacore::test::FakeToupTekSDK fake;
    fake.cameras.push_back(alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    fake.before_call = [&](const std::string& fn) {
        if (!armed.load()) return;
        if (fn == "stop" && in_restart.load()) abort_stop = true;  // the abort's stop
        if (fn == "put_binning") {
            in_restart = true;
            while (!abort_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (fn == "start_pull_mode") fake.hold_wait_image(true);  // a restarted stream waits for its frame
    };
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->set_bin_x(2);  // a real format change makes the next exposure restart the stream

    armed = true;
    driver->start_exposure(3.0, true);
    for (int i = 0; i < 5000 && !in_restart.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(in_restart.load());
    const auto t0 = Clock::now();
    driver->abort_exposure();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    armed = false;
    fake.release_wait_image();
    CHECK(ms < 1000);
    CHECK(fake.call_count("trigger") == 0);
    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - Disconnect during the stream restart is not undone",
          "[touptek][camera][unit][cc16]") {
    using Clock = std::chrono::steady_clock;
    std::atomic<bool> armed{false};
    std::atomic<bool> in_restart{false};
    std::atomic<bool> abort_stop{false};
    alpacacore::test::FakeToupTekSDK fake;
    fake.cameras.push_back(alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    fake.before_call = [&](const std::string& fn) {
        if (!armed.load()) return;
        if (fn == "stop" && in_restart.load()) abort_stop = true;  // the abort's stop
        if (fn == "put_binning") {
            in_restart = true;
            while (!abort_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (fn == "start_pull_mode") fake.hold_wait_image(true);  // a restarted stream waits for its frame
    };
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->set_bin_x(2);  // a real format change makes the next exposure restart the stream

    armed = true;
    driver->start_exposure(3.0, true);
    for (int i = 0; i < 5000 && !in_restart.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(in_restart.load());
    const auto t0 = Clock::now();
    driver->set_connected(false);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    armed = false;
    fake.release_wait_image();
    CHECK(ms < 1000);
    CHECK(fake.call_count("trigger") == 0);
}

// Decision 0020 rule 3. Falsified by: an idle AbortExposure or StopExposure that dismisses the kept failure, so
// ImageReady reads false instead of raising it.
TEST_CASE("ToupTek Camera Driver - a failed exposure survives AbortExposure and StopExposure",
          "[touptek][camera][unit][capture-session]") {
    alpacacore::test::FakeToupTekSDK fake;
    fake.cameras.push_back(alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->start_exposure(0.01, true);  // the fake delivers no frame: the exposure fails

    const auto image_ready_error = [&] {
        try {
            (void)driver->get_image_ready();
        } catch (const alpacacore::AlpacaException& e) {
            return static_cast<int>(e.error_code());
        }
        return 0;
    };
    const auto failure_kept = [&] {
        for (int i = 0; i < 400 && image_ready_error() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return image_ready_error() == static_cast<int>(alpacacore::AlpacaError::DriverException);
    };
    REQUIRE(failure_kept());

    driver->abort_exposure();
    CHECK(image_ready_error() == static_cast<int>(alpacacore::AlpacaError::DriverException));
    driver->stop_exposure();
    CHECK(image_ready_error() == static_cast<int>(alpacacore::AlpacaError::DriverException));
    require_alpaca_error([&] { (void)driver->get_image_array(); }, alpacacore::AlpacaError::DriverException);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);

    fake.deliver_frame = true;
    driver->start_exposure(0.01, true);  // the next StartExposure clears it
    CHECK(image_ready_error() != static_cast<int>(alpacacore::AlpacaError::DriverException));
    driver->set_connected(false);
}

namespace {

using namespace std::chrono_literals;

// Spins (real time, bounded) until `cond` holds; the driver's own threads
// make progress in the meantime. Virtual time never moves here.
template <typename Cond>
bool spin_until(Cond cond) {
    for (int i = 0; i < 5000 && !cond(); ++i) std::this_thread::sleep_for(1ms);
    return cond();
}

alpacacore::test::FakeToupTekSDK make_fake_camera(bool pulse_guide = false) {
    alpacacore::test::FakeToupTekSDK fake;
    auto info = alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One");
    info.supports_pulse_guide = pulse_guide;
    fake.cameras.push_back(info);
    return fake;
}

}  // namespace

TEST_CASE("ToupTek Camera Driver - thermal poll waits on the task clock", "[touptek][camera][unit][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    auto fake = make_fake_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // The first tick primes the cache at once, then the poller waits out the
    // interval on the clock.
    REQUIRE(spin_until([&] { return fake.call_count("get_temperature_deciC") >= 1; }));
    REQUIRE(clock.wait_for_wait_count(1, 5s));
    const int primed = fake.call_count("get_temperature_deciC");

    clock.advance(999ms);
    CHECK(fake.call_count("get_temperature_deciC") == primed);
    clock.advance(1ms);
    CHECK(spin_until([&] { return fake.call_count("get_temperature_deciC") > primed; }));
    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - thermal cache ages on the task clock", "[touptek][camera][unit][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    auto fake = make_fake_camera();
    fake.deliver_frame = false;
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake, clock);
    driver->set_connected(true);
    REQUIRE(spin_until([&] { return fake.call_count("get_temperature_deciC") >= 1; }));
    REQUIRE(clock.wait_for_wait_count(1, 5s));

    // The poller skips its reads while a frame integrates, so the cache ages.
    fake.hold_wait_image(true);
    driver->start_exposure(100.0, true);
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    for (int i = 0; i < 4; ++i) {
        clock.advance(1000ms);
        REQUIRE(clock.wait_for_woken_settled(5s));
    }
    const int before = fake.call_count("get_temperature_deciC");
    // Served as is during the exposure, however old.
    (void)driver->get_ccd_temperature();
    CHECK(fake.call_count("get_temperature_deciC") == before);

    driver->abort_exposure();
    fake.release_wait_image();
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Idle);
    // 4 s of clock time old: past the 3 s bound, so one live read ...
    (void)driver->get_ccd_temperature();
    CHECK(fake.call_count("get_temperature_deciC") == before + 1);
    // ... which stamps the cache fresh again.
    (void)driver->get_ccd_temperature();
    CHECK(fake.call_count("get_temperature_deciC") == before + 1);
    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - TEC cache ages on the task clock", "[touptek][camera][unit][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    auto fake = make_fake_camera();
    fake.deliver_frame = false;
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_can_get_cooler_power());
    REQUIRE(spin_until([&] { return fake.call_count("get_tec_voltage_deciV") >= 1; }));
    REQUIRE(clock.wait_for_wait_count(1, 5s));

    // The poller skips its reads while a frame integrates, so the cache ages.
    fake.hold_wait_image(true);
    driver->start_exposure(100.0, true);
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    for (int i = 0; i < 4; ++i) {
        clock.advance(1000ms);
        REQUIRE(clock.wait_for_woken_settled(5s));
    }
    const int before = fake.call_count("get_tec_voltage_deciV");
    (void)driver->get_cooler_power();
    CHECK(fake.call_count("get_tec_voltage_deciV") == before);

    driver->abort_exposure();
    fake.release_wait_image();
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Idle);
    // 4 s of clock time old: past the 3 s bound, so one live read ...
    (void)driver->get_cooler_power();
    CHECK(fake.call_count("get_tec_voltage_deciV") == before + 1);
    // ... which stamps the TEC cache fresh again.
    (void)driver->get_cooler_power();
    CHECK(fake.call_count("get_tec_voltage_deciV") == before + 1);
    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - exposure deadline reads the task clock", "[touptek][camera][unit][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    auto fake = make_fake_camera();
    fake.hold_wait_image(true);
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake, clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_wait_count(1, 5s));

    driver->start_exposure(1.0, true);  // deadline: 1 s exposure + 15 s grace
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    clock.advance(15999ms);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    clock.advance(1ms);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    fake.release_wait_image();
    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - pulse guide timer waits on the task clock", "[touptek][camera][unit][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    auto fake = make_fake_camera(true);
    fake.guiding = true;
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_can_pulse_guide());
    REQUIRE(clock.wait_for_wait_count(1, 5s));  // the thermal poller's wait

    driver->pulse_guide(0, 500);
    REQUIRE(clock.wait_for_wait_count(2, 5s));  // the pulse timer's wait
    CHECK(driver->get_is_pulse_guiding());
    clock.advance(499ms);
    CHECK(driver->get_is_pulse_guiding());
    clock.advance(1ms);
    CHECK(spin_until([&] { return !driver->get_is_pulse_guiding(); }));
    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - a superseding pulse cancels the clock wait", "[touptek][camera][unit][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    auto fake = make_fake_camera(true);
    fake.guiding = true;
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake, clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_wait_count(1, 5s));

    driver->pulse_guide(0, 10000);
    REQUIRE(clock.wait_for_wait_count(2, 5s));
    // No advance: replacing the pulse, disconnecting and destroying the driver
    // must all wake the wait. A regression in the cancel or its notify would
    // park the fake-clock wait for ever, in pulse_guide's join as much as in
    // disconnect or the destructor: run all three in one bounded async so the
    // case fails instead of hanging.
    struct Outcome {
        bool guiding;
        bool connected;
    };
    auto work = std::async(std::launch::async, [&]() -> Outcome {
        driver->pulse_guide(1, 10000);
        const bool guiding = driver->get_is_pulse_guiding();
        driver->set_connected(false);
        const bool connected = driver->get_connected();
        driver.reset();
        return {guiding, connected};
    });
    const bool ready = work.wait_for(5s) == std::future_status::ready;
    // Release every parked timer (the replaced pulse's and the new one's) so the
    // joins return and the future can be destroyed.
    for (int i = 0; !ready && i < 200 && work.wait_for(10ms) != std::future_status::ready; ++i) clock.advance(20s);
    REQUIRE(ready);
    const Outcome outcome = work.get();
    CHECK(outcome.guiding);
    CHECK_FALSE(outcome.connected);
}

TEST_CASE("ToupTek Camera Driver - a superseded pulse timer leaves the new pulse running",
          "[touptek][camera][unit][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    auto fake = make_fake_camera(true);
    fake.guiding = true;
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake, clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_wait_count(1, 5s));  // the thermal poller's wait

    driver->pulse_guide(0, 500);
    REQUIRE(clock.wait_for_wait_count(2, 5s));
    driver->pulse_guide(1, 2000);
    REQUIRE(clock.wait_for_wait_count(3, 5s));

    // The first pulse's 500 ms is up, but it was replaced: no stale end.
    clock.advance(500ms);
    REQUIRE(clock.wait_for_woken_settled(5s));
    CHECK(driver->get_is_pulse_guiding());
    clock.advance(1499ms);
    REQUIRE(clock.wait_for_woken_settled(5s));
    CHECK(driver->get_is_pulse_guiding());
    clock.advance(1ms);
    CHECK(spin_until([&] { return !driver->get_is_pulse_guiding(); }));
    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - disconnect mid-pulse reports no pulse and the destructor ends the parked timer",
          "[touptek][camera][unit][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    auto fake = make_fake_camera(true);
    fake.guiding = true;
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake, clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_wait_count(1, 5s));
    driver->pulse_guide(0, 10000);
    REQUIRE(clock.wait_for_wait_count(2, 5s));

    auto work = std::async(std::launch::async, [&] {
        driver->set_connected(false);
        const bool guiding = driver->get_is_pulse_guiding();
        driver.reset();  // the destructor cancels and joins the parked pulse timer
        return guiding;
    });
    const bool ready = work.wait_for(5s) == std::future_status::ready;
    for (int i = 0; !ready && i < 200 && work.wait_for(10ms) != std::future_status::ready; ++i) clock.advance(20s);
    REQUIRE(ready);
    CHECK_FALSE(work.get());
}

TEST_CASE("ToupTek Camera Driver - the thermal poll stops on disconnect and restarts on reconnect",
          "[touptek][camera][unit][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    auto fake = make_fake_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake, clock);
    driver->set_connected(true);
    REQUIRE(spin_until([&] { return fake.call_count("get_temperature_deciC") >= 1; }));
    REQUIRE(clock.wait_for_wait_count(1, 5s));

    driver->set_connected(false);
    const int after_disconnect = fake.call_count("get_temperature_deciC");
    REQUIRE(clock.waiter_count() == 0);  // the poller was joined: nothing is parked on the clock
    clock.advance(10s);
    CHECK(fake.call_count("get_temperature_deciC") == after_disconnect);

    driver->set_connected(true);
    REQUIRE(spin_until([&] { return fake.call_count("get_temperature_deciC") > after_disconnect; }));
    const int primed = fake.call_count("get_temperature_deciC");
    REQUIRE(clock.wait_for_wait_count(2, 5s));  // the restarted poller waits out its interval
    clock.advance(1000ms);
    CHECK(spin_until([&] { return fake.call_count("get_temperature_deciC") > primed; }));
    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - thermal reads racing a disconnect only fail NotConnected",
          "[touptek][camera][unit][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    auto fake = make_fake_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake, clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_wait_count(1, 5s));

    std::atomic<bool> stop{false};
    std::atomic<int> other_failures{0};
    std::thread reader([&] {
        while (!stop.load()) {
            try {
                (void)driver->get_ccd_temperature();
            } catch (const alpacacore::AlpacaException& e) {
                if (e.error_code() != alpacacore::AlpacaError::NotConnected) ++other_failures;
            } catch (...) {
                ++other_failures;
            }
            clock.advance(1000ms);  // ages the cache, so every read can reach the SDK
        }
    });
    for (int i = 0; i < 20; ++i) {
        driver->set_connected(false);
        driver->set_connected(true);
    }
    driver->set_connected(false);
    stop.store(true);
    reader.join();
    CHECK(other_failures.load() == 0);
}

// Issue #1277: the ToupTek SDK has no e-/ADU or full-well figure, so both raise PropertyNotImplemented (ICameraV4
// "if not available") instead of a made-up 1.0 or the ADU saturation; an unknown SensorName is "", not a placeholder.
TEST_CASE(
    "ToupTek Camera Driver - ElectronsPerADU and FullWellCapacity are not implemented, SensorName has no placeholder",
    "[touptek][camera][unit]") {
    alpacacore::test::FakeToupTekSDK fake;
    fake.cameras.push_back(alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);

    require_alpaca_error([&] { (void)driver->get_electrons_per_adu(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_full_well_capacity(); }, alpacacore::AlpacaError::NotConnected);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    require_alpaca_error([&] { (void)driver->get_electrons_per_adu(); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)driver->get_full_well_capacity(); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - an unknown SensorName is empty", "[touptek][camera][unit]") {
    alpacacore::test::FakeToupTekSDK fake;  // no camera attached
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    CHECK(driver->get_sensor_name().empty());
}

// ── Connected members over the SDK fake ─────────────────────────────────────

namespace {

using alpacacore::test::FakeToupTekSDK;

bool near(double actual, double expected) { return std::fabs(actual - expected) < 1e-6; }

FakeToupTekSDK fake_with_one_camera() {
    FakeToupTekSDK fake;
    fake.cameras.push_back(FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    return fake;
}

}  // namespace

TEST_CASE("ToupTek Camera Driver - Connected geometry and capability members report the fake's camera",
          "[touptek][camera][unit]") {
    // Falsified by: get_camera_y_size returning max_width, get_max_bin_x returning the smallest bin, get_max_adu
    // ignoring bit_depth_max, either pixel size returning the other axis, get_image_array_variant returning
    // "Int16", get_exposure_resolution returning 0.001, or get_sensor_type returning RGGB for a mono sensor.
    auto fake = fake_with_one_camera();
    fake.cameras[0].max_height = 1080;
    fake.pixel_size_um = {3.75F, 4.5F};
    fake.raw_format = {0x59595959, 12};  // mono 'YYYY', 12 bits
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);

    CHECK(driver->get_camera_y_size() == 1080);
    CHECK(driver->get_max_bin_x() == 4);
    CHECK(driver->get_max_bin_y() == 4);
    CHECK(driver->get_max_adu() == 4095);
    CHECK(near(driver->get_pixel_size_x(), 3.75));
    CHECK(near(driver->get_pixel_size_y(), 4.5));
    CHECK(driver->get_image_array_variant() == "Int32");
    CHECK(driver->get_exposure_resolution() == 0.000001);
    CHECK(driver->get_sensor_type() == alpacacore::SensorType::Monochrome);
    CHECK_FALSE(driver->get_can_fast_readout());
    CHECK(driver->get_can_set_ccd_temperature());

    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - Connected exposure and gain ranges come from the SDK in the ASCOM unit",
          "[touptek][camera][unit]") {
    // Falsified by: get_exposure_max or get_exposure_min skipping the microsecond-to-second division, swapping
    // min and max, or get_gain_min/get_gain_max reading the other bound.
    auto fake = fake_with_one_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);

    // The fake's range is 100 us .. 3600000000 us, gain 100 .. 300.
    CHECK(near(driver->get_exposure_min(), 0.0001));
    CHECK(near(driver->get_exposure_max(), 3600.0));
    CHECK(driver->get_gain_min() == 100);
    CHECK(driver->get_gain_max() == 300);

    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - Connected members the driver does not implement answer 0x400",
          "[touptek][camera][unit]") {
    // Falsified by: any of these returning a value, or throwing another code; a mono sensor has no Bayer
    // offsets either.
    auto fake = fake_with_one_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);

    const int not_implemented = alpacacore::AlpacaError::PropertyNotImplemented;
    require_alpaca_error([&] { (void)driver->get_fast_readout(); }, not_implemented);
    require_alpaca_error([&] { driver->set_fast_readout(true); }, not_implemented);
    require_alpaca_error([&] { (void)driver->get_gains(); }, not_implemented);
    require_alpaca_error([&] { (void)driver->get_bayer_offset_x(); }, not_implemented);
    require_alpaca_error([&] { (void)driver->get_bayer_offset_y(); }, not_implemented);

    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - A colour camera reports RGGB with the offsets its Bayer pattern implies",
          "[touptek][camera][unit]") {
    // Falsified by: swapping a row of the offset table (GR and GB then trade places), or get_sensor_type
    // returning Monochrome for a colour camera.
    struct Row {
        unsigned four_cc;
        int offset_x;
        int offset_y;
    };
    // The raw-format four-CC the SDK reports: RGGB, BGGR, GRBG, GBRG.
    const Row row =
        GENERATE(Row{0x42474752, 0, 0}, Row{0x52474742, 1, 1}, Row{0x47425247, 1, 0}, Row{0x47524247, 0, 1});
    CAPTURE(row.four_cc);

    auto fake = fake_with_one_camera();
    fake.raw_format = {row.four_cc, 16};
    fake.cameras[0].is_color = true;  // ADR 0015 rule 4: the enumerated colour flag agrees with the raw format
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);

    CHECK(driver->get_sensor_type() == alpacacore::SensorType::RGGB);
    CHECK(driver->get_bayer_offset_x() == row.offset_x);
    CHECK(driver->get_bayer_offset_y() == row.offset_y);

    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - Connected binning and start position keep their InvalidValue boundaries",
          "[touptek][camera][unit]") {
    // Falsified by: set_bin_x accepting a bin outside supported_bins (3, 0), get_bin_y not following the bin,
    // set_start_x/set_start_y accepting a negative value, or a rejected write changing the stored start.
    auto fake = fake_with_one_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);

    CHECK(driver->get_bin_x() == 1);
    CHECK(driver->get_bin_y() == 1);
    driver->set_bin_x(2);
    CHECK(driver->get_bin_x() == 2);
    CHECK(driver->get_bin_y() == 2);
    driver->set_bin_y(4);
    CHECK(driver->get_bin_x() == 4);
    CHECK(driver->get_bin_y() == 4);
    const int invalid = alpacacore::AlpacaError::InvalidValue;
    require_alpaca_error([&] { driver->set_bin_x(3); }, invalid);
    require_alpaca_error([&] { driver->set_bin_y(0); }, invalid);
    CHECK(driver->get_bin_x() == 4);

    driver->set_start_x(10);
    driver->set_start_y(20);
    CHECK(driver->get_start_x() == 10);
    CHECK(driver->get_start_y() == 20);
    require_alpaca_error([&] { driver->set_start_x(-1); }, invalid);
    require_alpaca_error([&] { driver->set_start_y(-1); }, invalid);
    CHECK(driver->get_start_x() == 10);
    CHECK(driver->get_start_y() == 20);

    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - Connected cooler members read and write the TEC registers",
          "[touptek][camera][unit]") {
    // Falsified by: set_cooler_on not reaching put_tec_enable, get_cooler_on not reading get_tec_enable,
    // set_set_ccd_temperature dropping the deci-degree scaling, get_set_ccd_temperature reading the live
    // temperature, or get_heat_sink_temperature returning something other than the sensor reading (21.0 C).
    auto fake = fake_with_one_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);

    CHECK_FALSE(driver->get_cooler_on());
    driver->set_cooler_on(true);
    CHECK(fake.call_count("put_tec_enable") == 1);
    CHECK(driver->get_cooler_on());
    driver->set_cooler_on(false);
    CHECK_FALSE(driver->get_cooler_on());

    driver->set_set_ccd_temperature(-10.4);
    CHECK(fake.call_count("put_tec_target_deciC") == 1);
    CHECK(near(driver->get_set_ccd_temperature(), -10.4));
    CHECK(near(driver->get_heat_sink_temperature(), 21.0));

    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - A camera without a TEC refuses cooler on", "[touptek][camera][unit]") {
    // Falsified by: set_cooler_on(true) silently succeeding, writing the TEC register of a camera without
    // one, or get_can_set_ccd_temperature returning true.
    auto fake = fake_with_one_camera();
    fake.cameras[0].supports_cooler = false;
    fake.cameras[0].supports_tec_onoff = false;
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);

    CHECK_FALSE(driver->get_can_set_ccd_temperature());
    require_alpaca_error([&] { driver->set_cooler_on(true); }, alpacacore::AlpacaError::NotImplemented);
    CHECK(fake.call_count("put_tec_enable") == 0);

    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - A camera without a cooler raises NotImplemented for CoolerOn and CoolerPower",
          "[touptek][camera][unit]") {
    // Falsified by: get_cooler_on answering false, set_cooler_on(false) returning quietly, get_cooler_power
    // answering 0.0, or any of them reading or writing a TEC register of a camera without a cooler.
    auto fake = fake_with_one_camera();
    fake.cameras[0].supports_cooler = false;
    fake.cameras[0].supports_tec_onoff = false;
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);

    const int not_implemented = alpacacore::AlpacaError::NotImplemented;
    CHECK_FALSE(driver->get_can_get_cooler_power());
    require_alpaca_error([&] { (void)driver->get_cooler_on(); }, not_implemented);
    require_alpaca_error([&] { driver->set_cooler_on(true); }, not_implemented);
    require_alpaca_error([&] { driver->set_cooler_on(false); }, not_implemented);
    require_alpaca_error([&] { (void)driver->get_cooler_power(); }, not_implemented);
    CHECK(fake.call_count("get_tec_enable") == 0);
    CHECK(fake.call_count("put_tec_enable") == 0);

    driver->set_connected(false);
}

TEST_CASE("ToupTek Camera Driver - Range and thermal members throw NotConnected while disconnected",
          "[touptek][camera][unit]") {
    // Falsified by: any of these answering a value, or another code, without a handle (ensure_connected()
    // alone is redundant with with_handle() for them: removing it does not change the answer).
    auto fake = fake_with_one_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);

    const int not_connected = alpacacore::AlpacaError::NotConnected;
    require_alpaca_error([&] { (void)driver->get_exposure_min(); }, not_connected);
    require_alpaca_error([&] { (void)driver->get_exposure_max(); }, not_connected);
    require_alpaca_error([&] { (void)driver->get_gain_min(); }, not_connected);
    require_alpaca_error([&] { (void)driver->get_gain_max(); }, not_connected);
    require_alpaca_error([&] { (void)driver->get_cooler_on(); }, not_connected);
    require_alpaca_error([&] { driver->set_cooler_on(true); }, not_connected);
    require_alpaca_error([&] { (void)driver->get_set_ccd_temperature(); }, not_connected);
    require_alpaca_error([&] { driver->set_set_ccd_temperature(0.0); }, not_connected);
    require_alpaca_error([&] { (void)driver->get_heat_sink_temperature(); }, not_connected);
}

// --- Exposure session (decision 0020) -------------------------------------

namespace {

// The Alpaca error a call raised, or 0 when it returned.
int error_of(const std::function<void()>& call) {
    try {
        call();
    } catch (const alpacacore::AlpacaException& e) {
        return e.error_code();
    }
    return 0;
}

// ImageReady, false while it raises.
bool frame_ready(const alpacacore::CameraDriver& driver) {
    try {
        return driver.get_image_ready();
    } catch (const alpacacore::AlpacaException&) {
        return false;
    }
}

}  // namespace

// Decision 0020 rule 3. Falsified by: a driver that clears the failure on a read, reports Exposing after it, or
// keeps it across the next StartExposure.
TEST_CASE("ToupTek Camera Driver - a failed exposure is kept until the next StartExposure",
          "[touptek][camera][unit][capture-session]") {
    auto fake = make_fake_camera();  // deliver_frame is false: the exposure fails
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->start_exposure(0.01, true);
    const int driver_error = alpacacore::AlpacaError::DriverException;
    REQUIRE(spin_until([&] { return error_of([&] { (void)driver->get_image_ready(); }) == driver_error; }));

    for (int poll = 0; poll < 3; ++poll) {
        CHECK(error_of([&] { (void)driver->get_image_ready(); }) == driver_error);
        CHECK(error_of([&] { (void)driver->get_image_array(); }) == driver_error);
        CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    }

    fake.hold_next_wait_image();
    driver->start_exposure(0.01, true);
    REQUIRE(fake.wait_for_held_image(5s));
    CHECK(error_of([&] { (void)driver->get_image_ready(); }) == 0);
    CHECK_FALSE(driver->get_image_ready());
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    driver->abort_exposure();
    driver->set_connected(false);
}

// Decision 0020 rule 5. Falsified by: an abort that joins without the SDK stop (the held wait never returns), or
// one that leaves the stopped stream for the next exposure to reuse.
TEST_CASE("ToupTek Camera Driver - AbortExposure during an exposure wakes the SDK wait and leaves the camera idle",
          "[touptek][camera][unit][capture-session]") {
    auto fake = make_fake_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    fake.hold_next_wait_image();
    driver->start_exposure(5.0, true);
    REQUIRE(fake.wait_for_held_image(5s));
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    const int stops = fake.call_count("stop");

    const auto t0 = std::chrono::steady_clock::now();
    driver->abort_exposure();
    CHECK(std::chrono::steady_clock::now() - t0 < 1s);
    CHECK(fake.call_count("stop") == stops + 1);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(error_of([&] { (void)driver->get_image_ready(); }) == 0);
    CHECK(error_of([&] { (void)driver->get_image_array(); }) == alpacacore::AlpacaError::InvalidOperation);

    // The body is gone, so the fake may change; the next exposure restarts the stopped stream.
    fake.deliver_frame = true;
    driver->start_exposure(0.01, true);
    CHECK(spin_until([&] { return frame_ready(*driver); }));
    CHECK(fake.call_count("start_pull_mode") == 2);
    driver->set_connected(false);
}

// Decision 0020 rules 5 and 8. Falsified by: a disconnect that closes the camera before the SDK stop and the join
// (the close lands while the wait is held), or one that keeps the last exposure across the reconnect.
TEST_CASE("ToupTek Camera Driver - a disconnect during an exposure stops the SDK wait before the close",
          "[touptek][camera][unit][capture-session]") {
    auto fake = make_fake_camera();
    std::atomic<bool> closed_while_held{false};
    fake.before_call = [&](const std::string& fn) {
        if (fn == "close_camera" && fake.wait_for_held_image(0ms)) closed_while_held = true;
    };
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    fake.hold_next_wait_image();
    driver->start_exposure(5.0, true);
    REQUIRE(fake.wait_for_held_image(5s));
    const int stops = fake.call_count("stop");

    const auto t0 = std::chrono::steady_clock::now();
    driver->set_connected(false);
    CHECK(std::chrono::steady_clock::now() - t0 < 1s);
    CHECK_FALSE(closed_while_held.load());
    CHECK(fake.call_count("stop") >= stops + 1);
    CHECK_FALSE(driver->get_connected());

    driver->set_connected(true);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK_FALSE(driver->get_image_ready());
    CHECK(error_of([&] { (void)driver->get_last_exposure_duration(); }) == alpacacore::AlpacaError::InvalidOperation);
    fake.deliver_frame = true;
    driver->start_exposure(0.01, true);
    CHECK(spin_until([&] { return frame_ready(*driver); }));
    driver->set_connected(false);
}

// Decision 0020 rule 2. Falsified by: a second StartExposure that supersedes the first (stops it and starts
// another trigger) instead of being refused.
TEST_CASE("ToupTek Camera Driver - a StartExposure during an exposure is refused and the first frame still arrives",
          "[touptek][camera][unit][capture-session]") {
    auto fake = make_fake_camera();
    fake.deliver_frame = true;
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    fake.hold_next_wait_image();
    driver->start_exposure(5.0, true);
    REQUIRE(fake.wait_for_held_image(5s));
    const int stops = fake.call_count("stop");

    CHECK(error_of([&] { driver->start_exposure(1.0, true); }) == alpacacore::AlpacaError::InvalidOperation);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    CHECK(fake.call_count("stop") == stops);
    CHECK(fake.call_count("trigger") == 1);

    fake.release_wait_image();
    REQUIRE(spin_until([&] { return frame_ready(*driver); }));
    CHECK(driver->get_last_exposure_duration() == 5.0);
    driver->set_connected(false);
}

// Decision 0020 rule 5. Falsified by: a body that fails and leaves the triggered stream running for the next
// StartExposure or AbortExposure to stop.
TEST_CASE("ToupTek Camera Driver - an exposure body that fails stops the stream it started",
          "[touptek][camera][unit][capture-session]") {
    auto fake = make_fake_camera();
    std::atomic<bool> fail_wait{true};
    fake.before_call = [&](const std::string& fn) {
        if (fn == "wait_image" && fail_wait.load()) {
            throw alpacacore::AlpacaException("injected image wait failure", alpacacore::AlpacaError::DriverException);
        }
    };
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    const int stops = fake.call_count("stop");
    driver->start_exposure(0.01, true);
    REQUIRE(spin_until([&] {
        return error_of([&] { (void)driver->get_image_ready(); }) == alpacacore::AlpacaError::DriverException;
    }));
    CHECK(fake.call_count("stop") == stops + 1);

    fail_wait = false;
    fake.deliver_frame = true;
    driver->start_exposure(0.01, true);
    CHECK(spin_until([&] { return frame_ready(*driver); }));
    CHECK(fake.call_count("start_pull_mode") == 2);  // the stopped stream was restarted
    driver->set_connected(false);
}

// Decision 0020 rules 2 and 5. Falsified by: a StartExposure that waits out the session's start timeout behind a
// body the watchdog left in the SDK wait, instead of stopping the stream to wake it.
TEST_CASE("ToupTek Camera Driver - a StartExposure wakes a body the watchdog left in the SDK wait",
          "[touptek][camera][unit][capture-session][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    auto fake = make_fake_camera();
    fake.deliver_frame = true;
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake, clock);
    driver->set_connected(true);
    fake.hold_next_wait_image();
    driver->start_exposure(1.0, true);
    REQUIRE(fake.wait_for_held_image(5s));
    clock.advance(16s);  // past 1 s + the 15 s margin
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Idle);
    REQUIRE(error_of([&] { (void)driver->get_image_ready(); }) == alpacacore::AlpacaError::DriverException);

    const auto t0 = std::chrono::steady_clock::now();
    CHECK(error_of([&] { driver->start_exposure(0.01, true); }) == 0);
    CHECK(std::chrono::steady_clock::now() - t0 < 1s);
    CHECK(spin_until([&] { return frame_ready(*driver); }));
    CHECK(driver->get_last_exposure_duration() == 0.01);
    driver->set_connected(false);
}

// Decision 0020 rule 5, issue #1995. The disconnect stops the stream and sets closing_ under mutex_, releases it,
// then closes the session in disconnect_and_join(). A StartExposure in that gap passes its connection check (Connected
// is still true) and the session is still open, so only closing_ refuses it; without it the start runs a body that
// parks in the SDK frame wait and holds the disconnect's join. The gap hook runs in that window. Falsified by:
// touptek_camera_driver.cpp start_exposure() without its closing_ check.
TEST_CASE("ToupTek Camera Driver - a StartExposure during a disconnect is refused and the disconnect returns promptly",
          "[touptek][camera][unit][capture-session]") {
    auto fake = make_fake_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const int triggers = fake.call_count("trigger");
    std::atomic<int> gap_calls{0};
    std::atomic<int> start_error{-1};
    alpacacore::vendor::touptek::set_disconnect_gap_hook_for_testing(*driver, [&] {
        if (gap_calls.fetch_add(1) != 0) return;
        start_error = error_of([&] { driver->start_exposure(0.1, true); });
    });
    fake.hold_next_wait_image();  // a body that got through would park in the frame wait

    const auto t0 = std::chrono::steady_clock::now();
    driver->set_connected(false);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    fake.release_wait_image();
    CHECK(gap_calls.load() == 1);
    CHECK(start_error.load() == static_cast<int>(alpacacore::AlpacaError::NotConnected));
    CHECK(fake.call_count("trigger") == triggers);
    CHECK(elapsed < 1s);
    CHECK_FALSE(driver->get_connected());
}

// The destructor's copy of the same window (issue #1995): it stops the stream and sets closing_ itself before
// disconnect_and_join(), while Connected is still true. Falsified by: touptek_camera_driver.cpp ~ToupTekCameraDriver()
// without its closing_ = true.
TEST_CASE("ToupTek Camera Driver - a StartExposure during the destructor's disconnect is refused",
          "[touptek][camera][unit][capture-session]") {
    auto fake = make_fake_camera();
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const int triggers = fake.call_count("trigger");
    alpacacore::CameraDriver* raw = driver.get();
    std::atomic<int> gap_calls{0};
    std::atomic<int> start_error{-1};
    // The destructor runs the hook, then its set_connected(false) runs it again; only the first one starts.
    alpacacore::vendor::touptek::set_disconnect_gap_hook_for_testing(*driver, [&] {
        if (gap_calls.fetch_add(1) != 0) return;
        start_error = error_of([&] { raw->start_exposure(0.1, true); });
    });
    fake.hold_next_wait_image();

    const auto t0 = std::chrono::steady_clock::now();
    driver.reset();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    fake.release_wait_image();
    CHECK(gap_calls.load() >= 1);
    CHECK(start_error.load() == static_cast<int>(alpacacore::AlpacaError::NotConnected));
    CHECK(fake.call_count("trigger") == triggers);
    CHECK(elapsed < 1s);
}

// Issue #1996. An AbortExposure that arrives while a StartExposure waits in session_.start() (behind a body the
// watchdog left running) was a no-op that reported success, and the new exposure then ran to its end. Falsified by:
// touptek_camera_driver.cpp stop_exposure() returning while exposure_starting_ without setting abort_pending_.
TEST_CASE("ToupTek Camera Driver - an AbortExposure during a StartExposure still in flight is not dropped",
          "[touptek][camera][unit][capture-session][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    auto fake = make_fake_camera();
    fake.deliver_frame = true;
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool hold_body = false;
    bool body_held = false;
    std::atomic<bool> watch_stop{false};
    std::atomic<bool> start_in_window{false};
    // The first exposure's body stops in put_exposure_us until released, so the session stays busy after the
    // watchdog fails that exposure.
    fake.before_call = [&](const std::string& fn) {
        if (fn == "stop" && watch_stop.load()) start_in_window = true;  // the second start's stop, under mutex_
        if (fn != "put_exposure_us") return;
        std::unique_lock<std::mutex> lock(gate_mutex);
        if (!hold_body) return;
        body_held = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return !hold_body; });
    };
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        hold_body = true;
    }
    driver->start_exposure(1.0, true);
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(gate_cv.wait_for(lock, 5s, [&] { return body_held; }));
    }
    clock.advance(16s);  // past 1 s + the 15 s margin: the watchdog fails it, the body is still running
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Idle);

    // The second StartExposure snapshots its settings, stops the stream and waits in session_.start().
    watch_stop = true;
    std::atomic<int> start_error{-1};
    std::thread starter([&] { start_error = error_of([&] { driver->start_exposure(0.5, true); }); });
    REQUIRE(spin_until([&] { return start_in_window.load(); }));
    // Queues on mutex_ behind the start's locked section, so it runs with exposure_starting_ set.
    CHECK(error_of([&] { driver->abort_exposure(); }) == 0);

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        hold_body = false;  // the first body finishes; the waiting start proceeds
    }
    gate_cv.notify_all();
    starter.join();
    CHECK(start_error.load() == 0);
    // The recorded abort ran once the start published Exposing: no frame from the aborted exposure.
    CHECK(spin_until([&] { return driver->get_camera_state() == alpacacore::CameraState::Idle; }));
    std::this_thread::sleep_for(200ms);  // room for an exposure the abort missed to publish its frame
    CHECK_FALSE(frame_ready(*driver));
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    driver->set_connected(false);
}

// --- Firmware and SDK version for the web UI (issue #1997) -----------------

// Falsified by: touptek_camera_driver.cpp get_device_firmware() returning std::nullopt (the AlpacaDriver default).
TEST_CASE("ToupTek Camera Driver - firmware and SDK version are served from the connect-time cache",
          "[touptek][camera][unit][firmware]") {
    auto fake = make_fake_camera();
    fake.sdk_version = "60.31488.20260519";
    fake.firmware_version = "4.1.2.20221031";
    fake.hw_version = "4.0";
    fake.fpga_version = "4.34";
    fake.production_date = "20260310";
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);

    // The library version is known before any connect; the camera's firmware is not.
    CHECK(driver->get_device_firmware() == std::nullopt);
    CHECK(driver->get_device_sdk_version() == std::optional<std::string>("60.31488.20260519"));

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_device_firmware() ==
          std::optional<std::string>("4.1.2.20221031 (hardware 4.0, FPGA 4.34, production date 2026-03-10)"));
    CHECK(driver->get_device_sdk_version() == std::optional<std::string>("60.31488.20260519"));
    // Neither goes in DriverInfo.
    CHECK(driver->get_driver_info() == "AlpacaCore ToupTek Camera Driver");

    // Cached: the getters reach no SDK call (decision 0016).
    const char* reads[] = {"get_sdk_version", "get_firmware_version", "get_hw_version", "get_fpga_version",
                           "get_production_date"};
    std::vector<int> before;
    for (const char* fn : reads) before.push_back(fake.call_count(fn));
    for (int i = 0; i < 3; ++i) {
        (void)driver->get_device_firmware();
        (void)driver->get_device_sdk_version();
    }
    for (std::size_t i = 0; i < before.size(); ++i) {
        INFO(reads[i]);
        CHECK(fake.call_count(reads[i]) == before[i]);
    }

    driver->set_connected(false);
    CHECK(driver->get_device_firmware() == std::nullopt);
    CHECK(driver->get_device_sdk_version() == std::optional<std::string>("60.31488.20260519"));
}

// A model that refuses a read (the wrapper answers an empty string) has that value left out. Falsified by:
// touptek_camera_driver.cpp compose_firmware() adding a part without its empty-value check.
TEST_CASE("ToupTek Camera Driver - a version the camera does not report is left out of the firmware string",
          "[touptek][camera][unit][firmware]") {
    auto fake = make_fake_camera();
    fake.firmware_version = "4.1.2.20221031";
    fake.hw_version = "4.0";
    fake.fpga_version = "";  // no FPGA on this model
    fake.production_date = "20260310";
    fake.sdk_version = "unknown";  // what the wrapper answers when Toupcam_Version() has no string
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    CHECK(driver->get_device_sdk_version() == std::nullopt);

    driver->set_connected(true);
    CHECK(driver->get_device_firmware() ==
          std::optional<std::string>("4.1.2.20221031 (hardware 4.0, production date 2026-03-10)"));
    driver->set_connected(false);

    // Only the firmware version: no parentheses.
    fake.hw_version = "";
    fake.production_date = "";
    driver->set_connected(true);
    CHECK(driver->get_device_firmware() == std::optional<std::string>("4.1.2.20221031"));
    driver->set_connected(false);

    // A read that throws (a fault, not a refusal) is left out too, and does not fail the connect.
    fake.hw_version = "4.0";
    fake.throw_from.insert("get_hw_version");
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_device_firmware() == std::optional<std::string>("4.1.2.20221031"));
    driver->set_connected(false);
    fake.throw_from.clear();

    // No firmware version, but a hardware and FPGA version: the details alone, with no parentheses.
    fake.firmware_version = "";
    fake.fpga_version = "4.34";
    driver->set_connected(true);
    CHECK(driver->get_device_firmware() == std::optional<std::string>("hardware 4.0, FPGA 4.34"));
    driver->set_connected(false);
    fake.fpga_version = "";

    // Nothing reported: no Firmware row at all.
    fake.firmware_version = "";
    fake.hw_version = "";
    driver->set_connected(true);
    CHECK(driver->get_device_firmware() == std::nullopt);
    driver->set_connected(false);
}

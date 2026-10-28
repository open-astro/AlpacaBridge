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
#include <functional>
#include <future>
#include <thread>

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

    CHECK_THROWS_AS(driver->get_ccd_temperature(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_gain(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_gain(100), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_offset(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->start_exposure(1.0, true), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->stop_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->abort_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->pulse_guide(0, 100), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_image_array(), alpacacore::AlpacaException);
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
    CHECK_THROWS_AS(driver->action("anything", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("", false), alpacacore::AlpacaException);
}

TEST_CASE("ToupTek Camera Driver - Sub-exposure not supported", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    CHECK_THROWS_AS(driver->get_sub_exposure_duration(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_sub_exposure_duration(1.0), alpacacore::AlpacaException);
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
    // (integer OffsetMin/OffsetMax mode), regardless of hardware.
    require_alpaca_error([&]() { driver->get_offset(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_offset(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offset_min(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offset_max(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offsets(); }, alpacacore::AlpacaError::PropertyNotImplemented);

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

TEST_CASE("ToupTek Camera Driver - AbortExposure clears a failure from a completed exposure",
          "[touptek][camera][unit]") {
    alpacacore::test::FakeToupTekSDK fake;
    fake.cameras.push_back(alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, fake);
    driver->set_connected(true);
    driver->start_exposure(0.01, true);  // the fake delivers no frame: the exposure fails

    const auto image_ready_fails = [&] {
        try {
            (void)driver->get_image_ready();
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    };
    bool failed = false;
    for (int i = 0; i < 400 && !failed; ++i) {
        failed = image_ready_fails();
        if (!failed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(failed);

    driver->abort_exposure();
    bool ready = true;
    CHECK_NOTHROW(ready = driver->get_image_ready());
    CHECK_FALSE(ready);
    CHECK_NOTHROW(driver->get_camera_state());
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
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

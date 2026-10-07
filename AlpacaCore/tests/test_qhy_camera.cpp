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
#include <alpacacore/vendor/qhy/qhy_camera_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_qhy_sdk.h"
#include "locked_qhy_sdk.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

using alpacacore::test::FakeQHYSDK;
using alpacacore::test::LockedQHYSDK;

// Connect coverage runs over the SDK seam (issue #321). The real QHY SDK
// cannot initialise on a test runner at all -- its libusb hotplug init
// segfaults on a USB-less host -- so before the seam existed nothing in this
// file ever reached set_connected(true).
// One-camera fake, shared with the other QHY seam test files (issue #342):
// this was three verbatim copies, so a change to what a default test fake
// looks like had to be made in three places with nothing failing if it was
// made in two.
FakeQHYSDK make_fake(const std::string& id = "fake-qhy-0") { return FakeQHYSDK::with_one_camera(id); }

using alpacacore::vendor::qhy::QHYWorker;

// Holds the first start of one worker between building its thread and storing
// it (issue #510), through the driver's test-only QHYWorkerStartHook. Declare
// it before the driver: the destructor releases a held start, so a failing
// case cannot leave the connector parked while the driver is destroyed.
class WorkerStartGate {
public:
    explicit WorkerStartGate(QHYWorker which) : which_(which) {}
    WorkerStartGate(const WorkerStartGate&) = delete;
    WorkerStartGate& operator=(const WorkerStartGate&) = delete;
    ~WorkerStartGate() { release(); }

    alpacacore::vendor::qhy::QHYWorkerStartHook hook() {
        return [this](QHYWorker worker) {
            if (worker != which_) {
                return;
            }
            std::unique_lock<std::mutex> lock(mutex_);
            if (held_) {
                return;  // only the first start of this worker is held
            }
            held_ = true;
            cv_.notify_all();
            cv_.wait(lock, [this] { return released_; });
        };
    }

    bool wait_until_held() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(5), [this] { return held_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    const QHYWorker which_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool held_ = false;
    bool released_ = false;
};

// Joins a helper thread on every exit path, releasing the gate first so the
// join cannot wait on a start the gate still holds.
struct JoinOnExit {
    WorkerStartGate& gate;
    std::thread& thread;
    ~JoinOnExit() {
        gate.release();
        if (thread.joinable()) {
            thread.join();
        }
    }
};

// Polls `done` for up to `timeout`; true as soon as it holds.
bool eventually(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds(4)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return done();
}

} // namespace

TEST_CASE("QHY Camera Driver - Defaults", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // Name is "QHY Camera" when no camera is plugged in, or the SDK name when detected
    CHECK(driver->get_name().find("QHY") != std::string::npos);
}

TEST_CASE("QHY Camera Driver - Device metadata", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(3, 1);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "QHY CCD Camera Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);  // ICameraV4 (Platform 7)
    CHECK(driver->get_unique_id() == "QHY_3");
}

TEST_CASE("QHY Camera Driver - Not connected throws", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    require_alpaca_error([&] { driver->get_ccd_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->get_heat_sink_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->get_cooler_on(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->get_set_ccd_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->get_cooler_power(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_set_ccd_temperature(0.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_cooler_on(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_cooler_on(false); }, alpacacore::AlpacaError::NotConnected);
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

TEST_CASE("QHY Camera Driver - Disconnected state", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_is_pulse_guiding() == false);
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == false);
    CHECK(driver->get_can_asymmetric_bin() == false);
    CHECK(driver->get_has_shutter() == false);
}

TEST_CASE("QHY Camera Driver - Unsupported actions", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("anything", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("", false), alpacacore::AlpacaException);
}

TEST_CASE("QHY Camera Driver - Sub-exposure not supported", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    CHECK_THROWS_AS(driver->get_sub_exposure_duration(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_sub_exposure_duration(1.0), alpacacore::AlpacaException);
}

TEST_CASE("QHY Camera Driver - ASCOM Error Codes", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

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

TEST_CASE("QHY Camera Driver - State Machine Contracts", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Idle);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == false);
}

// ── Connect path (over the SDK seam, issue #321) ────────────────────────────

TEST_CASE("QHY Camera Driver - Connects over the SDK seam", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // The connect sequence the wrapper documents: open, init, then chip info
    // and the readout-mode enumeration.
    CHECK(fake.physical_opens == 1);
    CHECK(fake.init_calls == 1);
    CHECK(fake.call_count("get_chip_info") == 1);
    CHECK(fake.call_count("get_num_readout_modes") == 1);
    CHECK(driver->get_camera_x_size() == 64);
    CHECK(driver->get_camera_y_size() == 48);
    CHECK(driver->get_readout_modes().size() == 2);

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_closes == 1);
    CHECK(fake.ref_count("fake-qhy-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Camera Driver - A failed connect rolls back the ref-counted open", "[qhy][camera][unit]") {
    // An unmatched open pins the handle for the life of the process: the CFW
    // driver shares the same open_count, so CloseQHYCCD would never fire.
    auto fake = make_fake();
    fake.throw_from.insert("set_bits_mode");
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 1);
    CHECK(fake.physical_closes == 1);  // rolled back
    CHECK(fake.ref_count("fake-qhy-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Camera Driver - Reconnect reuses the driver cleanly", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    driver->set_connected(true);
    driver->set_connected(false);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.physical_opens == 2);
    CHECK(fake.physical_closes == 1);

    driver->set_connected(false);
    CHECK(fake.physical_opens == fake.physical_closes);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Camera Driver - Connecting an unknown camera id fails and leaks nothing", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "not-a-camera", sdk);

    // DriverException, not NotConnected: the real OpenQHYCCD returning null
    // for an unrecognized id has no "not connected" concept, only "the open
    // failed" (matches QHYSDKWrapper::open_camera()).
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Camera Driver - An uninitialised SDK resource fails the connect", "[qhy][camera][unit]") {
    // The failure mode issue #321 is about: on real hardware this path is only
    // reachable by crashing the process inside libqhyccd.
    auto fake = make_fake();
    fake.sdk_resource_available = false;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 0);
}

TEST_CASE("QHY Camera Driver - Gain and offset round-trip while connected", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);

    driver->set_gain(42);
    CHECK(driver->get_gain() == 42);
    driver->set_offset(17);
    CHECK(driver->get_offset() == 17);
    CHECK(driver->get_gain_min() == 0);
    CHECK(driver->get_gain_max() == 100);

    require_alpaca_error([&]() { driver->set_gain(1000); }, alpacacore::AlpacaError::InvalidValue);

    driver->set_connected(false);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Camera Driver - Gain and offset the SDK cannot read throw DriverException", "[qhy][camera][unit]") {
    // Issue #510: get_param() answers the QHYCCD_ERROR sentinel (about 4.29e9)
    // for an unsupported control or a failed read, and static_cast<int> of a
    // double outside int's range is undefined behaviour -- in practice a
    // garbage Gain/Offset handed to the client as if the camera reported it.
    using alpacacore::vendor::qhy::control::GAIN;
    using alpacacore::vendor::qhy::control::OFFSET;
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // No worker threads run for an uncooled camera, so editing the fake
    // directly here does not race the driver.
    const auto require_driver_exception = [&](const std::function<int()>& read, const std::string& control_name) {
        try {
            const int value = read();
            FAIL("Expected AlpacaException, got " << value);
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()) == "QHY SDK returned no value for " + control_name);
        }
    };

    fake.params.erase(GAIN);
    fake.params.erase(OFFSET);
    require_driver_exception([&]() { return driver->get_gain(); }, "Gain");
    require_driver_exception([&]() { return driver->get_offset(); }, "Offset");

    // Not only the sentinel: any value no int can hold is refused, and so is
    // a non-finite one.
    fake.params[GAIN] = 1e12;
    fake.params[OFFSET] = -1e12;
    require_driver_exception([&]() { return driver->get_gain(); }, "Gain");
    require_driver_exception([&]() { return driver->get_offset(); }, "Offset");
    fake.params[GAIN] = std::numeric_limits<double>::quiet_NaN();
    fake.params[OFFSET] = std::numeric_limits<double>::infinity();
    require_driver_exception([&]() { return driver->get_gain(); }, "Gain");
    require_driver_exception([&]() { return driver->get_offset(); }, "Offset");

    // A readable value still converts.
    fake.params[GAIN] = 42.0;
    fake.params[OFFSET] = 17.0;
    CHECK(driver->get_gain() == 42);
    CHECK(driver->get_offset() == 17);

    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - Connecting by index with no cameras detected fails", "[qhy][camera][unit]") {
    // The empty-enumeration branch of resolve_camera_id_locked() -- unreachable
    // by the by-id factory, only exercisable through _by_index.
    FakeQHYSDK fake;  // no cameras added
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0, sdk);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 0);
}

TEST_CASE("QHY Camera Driver - Connecting by an out-of-range index fails and leaks nothing", "[qhy][camera][unit]") {
    auto fake = make_fake();  // exactly one camera, index 0
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 5, sdk);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::InvalidValue);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Camera Driver - Connecting by index resolves the id and connects", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0, sdk);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.physical_opens == 1);
    CHECK(fake.last_opened_id == "fake-qhy-0");

    driver->set_connected(false);
    CHECK(fake.physical_closes == 1);
}

TEST_CASE("QHY Camera Driver - an empty SDK version omits the DriverInfo suffix", "[qhy][camera][unit]") {
    // The real wrapper returns "" until the SDK resource comes up, which is
    // the branch get_driver_info()/get_device_sdk_version() special-case so
    // DriverInfo never renders a malformed "(SDK )". Reachable only because
    // the fake's version string is settable.
    auto fake = make_fake();
    fake.sdk_version = "";
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    CHECK(driver->get_driver_info() == "AlpacaCore QHY Camera Driver");
    CHECK_FALSE(driver->get_device_sdk_version().has_value());
}

TEST_CASE("QHY Camera Driver - a populated SDK version is surfaced in both places", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    CHECK(driver->get_driver_info() == "AlpacaCore QHY Camera Driver (SDK fake-qhy-1.0)");
    REQUIRE(driver->get_device_sdk_version().has_value());
    CHECK(*driver->get_device_sdk_version() == "fake-qhy-1.0");
}

TEST_CASE("QHY Camera Driver - disconnect does not wait out the telemetry sleep", "[qhy][camera][unit]") {
    // open-astro#323. The telemetry worker polls CURTEMP once a second and
    // used a bare sleep_for, which is not interruptible, so setting the stop
    // flag did nothing until the current second elapsed. Every Connected=false
    // on a COOLED camera therefore blocked up to ~1 s -- most of ConformU's
    // 1.0 s STANDARD budget for that call, spent on a sleep the driver had
    // already decided to abandon, leaving none for the SDK close on a slow
    // device.
    //
    // has_cooler is the sole condition that starts the thread, so the cooled
    // fake is the one that reproduces it.
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // Let the worker reach its wait rather than catching it mid-iteration, so
    // this measures the wait and not the startup.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const auto started = std::chrono::steady_clock::now();
    driver->set_connected(false);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    REQUIRE_FALSE(driver->get_connected());

    // Generous on purpose: the point is that it is not ~1 s, not that it is
    // any particular small number -- a tight bound here would only buy CI
    // flakes on a loaded box. Before the fix this sat just under 1000 ms;
    // the wait is now woken immediately.
    CHECK(elapsed < std::chrono::milliseconds(600));
}

TEST_CASE("QHY Camera Driver - disconnect does not wait out the temperature worker's sleep", "[qhy][camera][unit]") {
    // The other polling worker #323 changed. cooler_on_ is constructed true
    // and the temp-control worker starts on connect whenever has_cooler and
    // cooler_on_ both hold, so the telemetry case above already runs it
    // (reverting only the temp worker's wait_for to sleep_for turns that
    // case red at ~870 ms); this case says so explicitly and pins it through
    // the public setter, so the coverage does not hinge on a constructor
    // default a later change could flip.
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->set_cooler_on(true);
    REQUIRE(driver->get_cooler_on());

    // Let both workers reach their waits rather than catching one
    // mid-iteration, so this measures the wait and not the startup.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const auto started = std::chrono::steady_clock::now();
    driver->set_connected(false);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    REQUIRE_FALSE(driver->get_connected());
    CHECK(elapsed < std::chrono::milliseconds(600));
}

TEST_CASE("QHY Camera Driver - a disconnect inside a telemetry start does not strand telemetry",
          "[qhy][camera][unit]") {
    // Issue #510. start_telemetry_thread() publishes the new generation's stop
    // flag under mutex_ but stores the thread only at the end. A disconnect in
    // between moved out an empty telemetry_thread_ and stopped the generation;
    // the start then stored a joinable thread whose worker had already exited,
    // and every later start returned at the joinable() guard, so CCDTemperature
    // never updated again for the life of the driver.
    using alpacacore::vendor::qhy::control::CURTEMP;
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    // The temp worker ramps CURTEMP toward the setpoint; a negligible step
    // keeps the value this case plants readable.
    fake.temp_settle_step_c = 1e-9;
    LockedQHYSDK sdk(fake);
    WorkerStartGate gate(QHYWorker::Telemetry);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, gate.hook());

    std::thread connector([&]() { driver->set_connected(true); });
    JoinOnExit join_connector{gate, connector};
    REQUIRE(gate.wait_until_held());
    REQUIRE(driver->get_connected());  // connected_ is stored before the workers start

    driver->set_connected(false);
    gate.release();
    connector.join();
    REQUIRE_FALSE(driver->get_connected());

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    // Planted after the reconnect, so only a worker of the new generation can
    // report it: neither the setpoint fallback (0.0) nor the first
    // generation's reading (-5.0) matches.
    sdk.set_param("fake-qhy-0", CURTEMP, 12.5);
    // Until the new worker's first poll lands the getter raises InvalidOperation
    // instead of answering a placeholder, so a throw counts as "not yet".
    CHECK(eventually([&]() {
        try {
            return std::abs(driver->get_ccd_temperature() - 12.5) < 1e-3;
        } catch (const alpacacore::AlpacaException&) {
            return false;
        }
    }));

    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - a disconnect inside a temperature-worker start does not strand it",
          "[qhy][camera][unit]") {
    // Issue #510, the temp-control worker: same publish-then-store shape as
    // telemetry (and the cooler-off worker moves temp_thread_ out too), so a
    // stop landing in the window stranded cooler regulation the same way.
    using alpacacore::vendor::qhy::control::CURTEMP;
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    LockedQHYSDK sdk(fake);
    WorkerStartGate gate(QHYWorker::TempControl);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, gate.hook());

    std::thread connector([&]() { driver->set_connected(true); });
    JoinOnExit join_connector{gate, connector};
    REQUIRE(gate.wait_until_held());
    REQUIRE(driver->get_connected());

    driver->set_connected(false);
    gate.release();
    connector.join();
    REQUIRE_FALSE(driver->get_connected());

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_cooler_on());
    // Only the temp worker calls control_temp(), which steps CURTEMP toward
    // the setpoint (0.0) on every call; nothing else writes it. A live worker
    // moves the planted value within a second or so.
    sdk.set_param("fake-qhy-0", CURTEMP, 10.0);
    CHECK(eventually([&]() { return sdk.get_param("fake-qhy-0", CURTEMP) < 10.0; }));

    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - BinX write during an exposure fails at once", "[qhy][camera][unit][cc05]") {
    std::atomic<bool> armed{false};
    std::atomic<bool> in_frame{false};
    std::atomic<bool> release{false};
    auto fake = alpacacore::test::FakeQHYSDK::with_one_camera();
    // Model the production wrapper: the frame download holds the per-handle call
    // mutex for the whole frame (LockedQHYSDK's one mutex plays that role).
    fake.before_call = [&](const std::string& fn) {
        if (armed.load() && fn == "get_single_frame") {
            in_frame = true;
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    alpacacore::test::LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    armed = true;
    driver->start_exposure(1.0, true);
    REQUIRE(eventually([&]() { return in_frame.load(); }));

    std::atomic<int> bin_code{-2};  // -2 = still blocked
    std::thread binner([&] {
        try {
            driver->set_bin_x(2);
            bin_code = -1;
        } catch (const alpacacore::AlpacaException& e) {
            bin_code = e.error_code();
        }
    });
    std::atomic<bool> state_done{false};
    std::thread reader([&] {
        (void)driver->get_camera_state();
        state_done = true;
    });
    const bool state_ok = eventually([&]() { return state_done.load(); });
    const bool bin_done = eventually([&]() { return bin_code.load() != -2; });
    const int code = bin_code.load();
    release = true;
    binner.join();
    reader.join();
    CHECK(state_ok);
    CHECK(bin_done);
    CHECK(code == alpacacore::AlpacaError::InvalidOperation);
    driver->set_connected(false);
}

namespace {
int error_code_of(const std::function<void()>& fn) {
    try {
        fn();
        return 0;
    } catch (const alpacacore::AlpacaException& ex) {
        return ex.error_code();
    }
}
}  // namespace

TEST_CASE("QHY Camera Driver - a failed exposure is raised by ImageReady and ImageArray", "[qhy][camera][unit]") {
    auto fake = make_fake();
    fake.frame_ok = false;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    driver->start_exposure(0.1, true);
    REQUIRE(eventually([&] { return error_code_of([&] { (void)driver->get_image_ready(); }) != 0; }));
    CHECK(error_code_of([&] { (void)driver->get_image_ready(); }) == alpacacore::AlpacaError::DriverException);
    CHECK(error_code_of([&] { (void)driver->get_image_array(); }) == alpacacore::AlpacaError::DriverException);
    // The next StartExposure clears the stored failure.
    fake.frame_ok = true;
    driver->start_exposure(0.05, true);
    CHECK(eventually([&] { return error_code_of([&] { (void)driver->get_image_ready(); }) == 0; }));
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - a watchdog timeout is raised by ImageReady and ImageArray without CameraState",
          "[qhy][camera][unit]") {
    std::atomic<bool> in_frame{false};
    std::atomic<bool> release{false};
    auto fake = make_fake();
    fake.before_call = [&](const std::string& fn) {
        if (fn == "get_single_frame") {
            in_frame = true;
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, {}, std::chrono::seconds(1));
    driver->set_connected(true);
    driver->start_exposure(0.05, true);
    REQUIRE(eventually([&] { return in_frame.load(); }));
    // Only the two image getters are polled: CameraState is never read.
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    const int ready_code = error_code_of([&] { (void)driver->get_image_ready(); });
    const int array_code = error_code_of([&] { (void)driver->get_image_array(); });
    release = true;
    CHECK(ready_code == alpacacore::AlpacaError::DriverException);
    CHECK(array_code == alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - StopExposure is not supported and AbortExposure keeps a finished frame",
          "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    CHECK_FALSE(driver->get_can_stop_exposure());
    CHECK(driver->get_can_abort_exposure());
    require_alpaca_error([&] { driver->stop_exposure(); }, alpacacore::AlpacaError::MethodNotImplemented);
    driver->start_exposure(0.05, true);
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));
    driver->abort_exposure();
    CHECK(driver->get_image_ready());
    CHECK(error_code_of([&] { (void)driver->get_last_exposure_duration(); }) == 0);
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - HeatSinkTemperature, ElectronsPerADU and FullWellCapacity are not implemented",
          "[qhy][camera][unit]") {
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    REQUIRE(eventually([&] { return error_code_of([&] { (void)driver->get_ccd_temperature(); }) == 0; }));
    require_alpaca_error([&] { (void)driver->get_heat_sink_temperature(); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)driver->get_electrons_per_adu(); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)driver->get_full_well_capacity(); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    for (const auto& s : driver->get_device_state()) {
        CHECK(s.name != "HeatSinkTemperature");
        CHECK(s.name != "ElectronsPerADU");
        CHECK(s.name != "FullWellCapacity");
    }
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - CCDTemperature is not invented", "[qhy][camera][unit]") {
    SECTION("uncooled camera") {
        auto fake = make_fake();
        LockedQHYSDK sdk(fake);
        auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
        driver->set_connected(true);
        require_alpaca_error([&] { (void)driver->get_ccd_temperature(); },
                             alpacacore::AlpacaError::PropertyNotImplemented);
        for (const auto& s : driver->get_device_state()) CHECK(s.name != "CCDTemperature");
        driver->set_connected(false);
    }
    SECTION("cooled camera without a CURTEMP control") {
        auto fake = FakeQHYSDK::with_one_cooled_camera();
        fake.controls_available.erase(alpacacore::vendor::qhy::control::CURTEMP);
        fake.params.erase(alpacacore::vendor::qhy::control::CURTEMP);
        LockedQHYSDK sdk(fake);
        auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
        driver->set_connected(true);
        CHECK(eventually([&] {
            return error_code_of([&] { (void)driver->get_ccd_temperature(); }) ==
                   alpacacore::AlpacaError::PropertyNotImplemented;
        }));
        driver->set_connected(false);
    }
    SECTION("cooled camera whose CURTEMP read fails") {
        auto fake = FakeQHYSDK::with_one_cooled_camera();
        fake.throw_from.insert("get_param");
        LockedQHYSDK sdk(fake);
        auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
        driver->set_connected(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(error_code_of([&] { (void)driver->get_ccd_temperature(); }) == alpacacore::AlpacaError::InvalidOperation);
        driver->set_connected(false);
        fake.throw_from.clear();
    }
}

TEST_CASE("QHY Camera Driver - StartExposure above ExposureMax is InvalidValue", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    require_alpaca_error([&] { driver->start_exposure(driver->get_exposure_max() + 1000.0, true); },
                         alpacacore::AlpacaError::InvalidValue);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - ImageArray during an exposure is InvalidOperation at once", "[qhy][camera][unit]") {
    std::atomic<bool> armed{false};
    std::atomic<bool> in_frame{false};
    std::atomic<bool> release{false};
    auto fake = make_fake();
    fake.before_call = [&](const std::string& fn) {
        if (armed.load() && fn == "get_single_frame") {
            in_frame = true;
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    armed = true;
    driver->start_exposure(1.0, true);
    REQUIRE(eventually([&] { return in_frame.load(); }));
    const auto t0 = std::chrono::steady_clock::now();
    const int code = error_code_of([&] { (void)driver->get_image_array(); });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    release = true;
    CHECK(code == alpacacore::AlpacaError::InvalidOperation);
    CHECK(ms.count() < 1000);
    driver->set_connected(false);
}

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

#include "camera_percent_cooler_checks.h"
#include "catch2_compat.h"
#include "fake_qhy_sdk.h"
#include "fake_task_clock.h"
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
namespace AlpacaError = alpacacore::AlpacaError;

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

    for (double duration : {-1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        require_alpaca_error([&] { driver->start_exposure(duration, true); }, AlpacaError::InvalidValue);
    }

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
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

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
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

TEST_CASE("QHY Camera Driver - CCDTemperature is readable right after connect", "[qhy][camera][unit]") {
    // open-astro#941. The telemetry worker's first CURTEMP poll lands after
    // Connected=true returns, so a read in that window failed with
    // InvalidOperation. Connect now seeds the cache with one CURTEMP read.
    // open-astro#1307: connect also starts the temperature-control worker
    // (cooler on, target 0 C), which steps CURTEMP toward 0 and the telemetry
    // worker then overwrites the seed, so a slow build read -4.5 for -5.0.
    // A negligible step (0.0 would settle at once) keeps CURTEMP at the seeded
    // value, as the neighbouring case at the telemetry restart does.
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    fake.temp_settle_step_c = 1e-9;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    CHECK(driver->get_ccd_temperature() == Catch::Approx(-5.0));
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - a failed connect-time CURTEMP read keeps CCDTemperature an error",
          "[qhy][camera][unit]") {
    // open-astro#941. The seed read failing must neither fail the connect nor
    // be replaced by the setpoint or a fixed number.
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    fake.throw_from.insert("get_param");
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    CHECK_THROWS_AS(driver->get_ccd_temperature(), alpacacore::AlpacaException);
    driver->set_connected(false);
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

// ICameraV4: LastExposure* raise InvalidOperation until an image has been successfully acquired (issue #1477).
TEST_CASE("QHY Camera Driver - LastExposure values follow successful images only", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    const auto failed = [&] { return error_code_of([&] { (void)driver->get_image_ready(); }) != 0; };

    SECTION("before any exposure") { alpacacore::test::require_last_exposure_unset(*driver); }
    SECTION("a failed first exposure publishes nothing") {
        fake.frame_ok = false;
        driver->start_exposure(0.01, true);
        REQUIRE(eventually(failed));
        alpacacore::test::require_last_exposure_unset(*driver);
    }
    SECTION("a later failure keeps the earlier success") {
        driver->start_exposure(0.05, true);
        REQUIRE(eventually([&] { return driver->get_image_ready(); }));
        const auto start = driver->get_last_exposure_start_time();
        CHECK(driver->get_last_exposure_duration() == 0.05);
        fake.frame_ok = false;
        driver->start_exposure(0.07, true);
        REQUIRE(eventually(failed));
        CHECK(driver->get_last_exposure_duration() == 0.05);
        CHECK(driver->get_last_exposure_start_time() == start);
    }
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - AbortExposure clears a failure from a completed exposure", "[qhy][camera][unit]") {
    auto fake = make_fake();
    fake.frame_ok = false;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    driver->start_exposure(0.1, true);
    REQUIRE(eventually([&] { return error_code_of([&] { (void)driver->get_image_ready(); }) != 0; }));

    driver->abort_exposure();
    bool ready = true;
    CHECK_NOTHROW(ready = driver->get_image_ready());
    CHECK_FALSE(ready);
    CHECK_NOTHROW(driver->get_camera_state());
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - malformed SDK frame never publishes ImageReady and recovers", "[qhy][camera][unit]") {
    auto fake = make_fake();
    fake.frame_width = 0;
    fake.read_directly = true;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    driver->start_exposure(0.05, true);

    REQUIRE(eventually([&] {
        return error_code_of([&] { (void)driver->get_image_ready(); }) == alpacacore::AlpacaError::DriverException;
    }));
    const int array_error = error_code_of([&] { (void)driver->get_image_array(); });
    INFO("ImageArray error=" << array_error << "; camera state=" << static_cast<int>(driver->get_camera_state()));
    CHECK(array_error == alpacacore::AlpacaError::DriverException);

    fake.frame_width.reset();
    fake.frame_bpp = 12;
    driver->start_exposure(0.05, true);
    REQUIRE(eventually([&] {
        return error_code_of([&] { (void)driver->get_image_ready(); }) == alpacacore::AlpacaError::DriverException;
    }));

    fake.frame_bpp.reset();
    fake.mem_length_override = 1;
    driver->start_exposure(0.05, true);
    REQUIRE(eventually([&] {
        return error_code_of([&] { (void)driver->get_image_ready(); }) == alpacacore::AlpacaError::DriverException;
    }));

    fake.mem_length_override.reset();
    driver->start_exposure(0.05, true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (const alpacacore::AlpacaException&) {
            return false;
        }
    }));
    const auto image = driver->get_image_array();
    CHECK(image.rank == 2);
    CHECK(image.width > 0);
    CHECK(image.height > 0);
    CHECK(image.data.size() == static_cast<std::size_t>(image.width) * image.height);
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - malformed exposure failure clears on reconnect", "[qhy][camera][unit]") {
    auto fake = make_fake();
    fake.frame_width = 0;
    fake.read_directly = true;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    driver->start_exposure(0.05, true);
    REQUIRE(eventually([&] {
        return error_code_of([&] { (void)driver->get_image_ready(); }) == alpacacore::AlpacaError::DriverException;
    }));

    driver->set_connected(false);
    driver->set_connected(true);
    CHECK_FALSE(driver->get_image_ready());
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - positive short SDK frame is zero-padded to requested ROI", "[qhy][camera][unit]") {
    auto fake = make_fake();
    fake.frame_width = 2;
    fake.frame_height = 2;
    fake.frame_bytes = {1, 0, 2, 0, 3, 0, 4, 0};  // little-endian uint16, as the SDK delivers
    fake.read_directly = true;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    driver->set_num_x(4);
    driver->set_num_y(3);
    driver->start_exposure(0.05, true);

    REQUIRE(eventually([&] { return driver->get_image_ready(); }));
    const auto image = driver->get_image_array();
    CHECK(image.width == 4);
    CHECK(image.height == 3);
    CHECK(image.rank == 2);
    CHECK(image.data == std::vector<std::int32_t>{1, 2, 0, 0, 3, 4, 0, 0, 0, 0, 0, 0});
    driver->set_connected(false);
}

// The SDK writes native little-endian uint16 samples; the driver once read them
// high byte first, which swapped every pixel of a real QHY5III585M frame into
// noise (ringed stars, nothing for a star detector to find). Values above 255
// make the two byte orders distinguishable -- {1,0} reads as 1 either way only
// when the high byte is zero, so this case uses a full 16-bit spread.
TEST_CASE("QHY Camera Driver - 16-bit samples are read little-endian (low byte first)", "[qhy][camera][unit]") {
    auto fake = make_fake();
    fake.frame_width = 2;
    fake.frame_height = 2;
    fake.frame_bytes = {0x34, 0x12, 0xFF, 0xFF, 0x00, 0x80, 0x01, 0x00};
    fake.read_directly = true;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    driver->set_num_x(2);
    driver->set_num_y(2);
    driver->start_exposure(0.05, true);
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));
    const auto image = driver->get_image_array();
    CHECK(image.width == 2);
    CHECK(image.height == 2);
    CHECK(image.data == std::vector<std::int32_t>{0x1234, 65535, 32768, 1});
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - ImageArray keeps the completed frame's NumX after NumX changes", "[qhy][camera][unit]") {
    auto fake = make_fake();
    fake.frame_width = 2;
    fake.frame_height = 2;
    fake.frame_bytes = {1, 0, 2, 0, 3, 0, 4, 0};  // little-endian uint16, as the SDK delivers
    fake.read_directly = true;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    driver->set_num_x(4);
    driver->set_num_y(3);
    driver->start_exposure(0.05, true);
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));

    driver->set_num_x(2);
    const auto image = driver->get_image_array();
    CHECK(image.width == 4);
    CHECK(image.height == 3);
    CHECK(image.data == std::vector<std::int32_t>{1, 2, 0, 0, 3, 4, 0, 0, 0, 0, 0, 0});
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - ImageArray keeps the completed frame's NumY after NumY changes", "[qhy][camera][unit]") {
    auto fake = make_fake();
    fake.frame_width = 2;
    fake.frame_height = 2;
    fake.frame_bytes = {1, 0, 2, 0, 3, 0, 4, 0};  // little-endian uint16, as the SDK delivers
    fake.read_directly = true;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    driver->set_num_x(4);
    driver->set_num_y(3);
    driver->start_exposure(0.05, true);
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));

    driver->set_num_y(1);
    const auto image = driver->get_image_array();
    CHECK(image.width == 4);
    CHECK(image.height == 3);
    CHECK(image.data == std::vector<std::int32_t>{1, 2, 0, 0, 3, 4, 0, 0, 0, 0, 0, 0});
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - aborting a pending download leaves ImageReady false without an error",
          "[qhy][camera][unit]") {
    auto fake = make_fake();
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool download_entered = false;
    bool release_download = false;
    fake.before_call = [&](const std::string& fn) {
        std::unique_lock<std::mutex> lock(gate_mutex);
        if (fn == "get_single_frame") {
            download_entered = true;
            gate_cv.notify_all();
            gate_cv.wait(lock, [&] { return release_download; });
        } else if (fn == "cancel_exposure") {
            release_download = true;
            gate_cv.notify_all();
        }
    };
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    driver->start_exposure(0.05, true);
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        CHECK(gate_cv.wait_for(lock, std::chrono::seconds(2), [&] { return download_entered; }));
    }
    driver->abort_exposure();
    CHECK_FALSE(driver->get_image_ready());
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
    const auto code_of_ready = [&] { return error_code_of([&] { (void)driver->get_image_ready(); }); };
    const auto code_of_array = [&] { return error_code_of([&] { (void)driver->get_image_array(); }); };
    REQUIRE(eventually([&] { return code_of_ready() == alpacacore::AlpacaError::DriverException; },
                       std::chrono::seconds(5)));
    CHECK(code_of_array() == alpacacore::AlpacaError::DriverException);
    // The frame arrives late: the stored failure must stand, not turn into a
    // ready image without a new StartExposure.
    release = true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    bool stayed_failed = true;
    while (std::chrono::steady_clock::now() < until) {
        stayed_failed = stayed_failed && code_of_ready() == alpacacore::AlpacaError::DriverException &&
                        code_of_array() == alpacacore::AlpacaError::DriverException;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(stayed_failed);
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

// Issue #1277: SensorName is "" when the chip info could not be read, never a "QHY Sensor" placeholder.
TEST_CASE("QHY Camera Driver - SensorName is empty when chip info is unreadable", "[qhy][camera][unit]") {
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    fake.chip_info_available = false;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.call_count("get_chip_info") == 1);
    CHECK(driver->get_sensor_name().empty());
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
    SECTION("a good reading is dropped once the CURTEMP read starts failing") {
        std::atomic<bool> fail_reads{false};
        auto fake = FakeQHYSDK::with_one_cooled_camera();
        fake.before_call = [&](const std::string& fn) {
            if (fail_reads.load() && fn == "get_param") {
                throw alpacacore::AlpacaException("fake: injected failure in get_param",
                                                  alpacacore::AlpacaError::DriverException);
            }
        };
        LockedQHYSDK sdk(fake);
        auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
        driver->set_connected(true);
        REQUIRE(eventually([&] { return error_code_of([&] { (void)driver->get_ccd_temperature(); }) == 0; }));
        fail_reads = true;
        CHECK(eventually([&] {
            return error_code_of([&] { (void)driver->get_ccd_temperature(); }) ==
                   alpacacore::AlpacaError::InvalidOperation;
        }));
        driver->set_connected(false);
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
    int code = 0;
    bool asked_too_early = false;
    try {
        (void)driver->get_image_array();
    } catch (const alpacacore::AlpacaException& e) {
        code = e.error_code();
        asked_too_early = e.is_asked_too_early();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    release = true;
    CHECK(code == alpacacore::AlpacaError::InvalidOperation);
    // Issue #1167: the "poll ImageReady" answer is logged at DEBUG, not ERROR.
    CHECK(asked_too_early);
    CHECK(ms.count() < 1000);
    driver->set_connected(false);
}

// ---------------------------------------------------------------------------
// Waits and deadlines on a FakeTaskClock (issue #1386, decision 0005). Each
// case below drives one timed site of the camera driver with advance() and
// never sleeps for it. Declare the clock before the driver: it must outlive
// the driver and any worker the driver detaches.
// ---------------------------------------------------------------------------
namespace {

using alpacacore::test::FakeTaskClock;
using std::chrono::milliseconds;
using std::chrono::seconds;

// Holds one SDK call open until released, through FakeQHYSDK::before_call.
struct SdkGate {
    std::string fn;
    std::atomic<bool> armed{true};
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};

    void arm(FakeQHYSDK& fake, std::string call) {
        fn = std::move(call);
        fake.before_call = [this](const std::string& name) {
            if (armed.load() && name == fn) {
                entered = true;
                while (!release.load()) {
                    std::this_thread::sleep_for(milliseconds(1));
                }
            }
        };
    }
};

// Collects the QHY log lines of one case (workers log from their own threads).
struct QhyLogCapture {
    std::mutex mutex;
    std::vector<std::string> lines;
    alpacacore::logging::LogSink previous_sink = alpacacore::logging::get_log_sink();

    QhyLogCapture() {
        alpacacore::logging::set_log_sink([this](auto, auto component, auto message) {
            if (component == "QHY") {
                std::lock_guard<std::mutex> lock(mutex);
                lines.emplace_back(message);
            }
        });
    }
    ~QhyLogCapture() { alpacacore::logging::set_log_sink(previous_sink); }

    std::string find(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& line : lines) {
            if (line.find(needle) != std::string::npos) {
                return line;
            }
        }
        return {};
    }
};

// Advances virtual time 20 ms at a time, in lock step with the woken threads,
// until `done` holds; returns the virtual time spent. This is what lets a
// reap loop (which sleeps 20 ms per pass on the clock) make progress. Each
// step also yields `pace` of real time: a worker that is about to finish needs
// real time to do so, and without it the loop would run 2 s of virtual reap
// bound out in microseconds and detach a worker that was seconds from done.
milliseconds step_until(FakeTaskClock& clock, const std::function<bool()>& done,
                        std::chrono::microseconds pace = std::chrono::milliseconds(2), int max_steps = 400) {
    milliseconds spent{0};
    for (int i = 0; i < max_steps && !done(); ++i) {
        clock.advance(milliseconds(20));
        spent += milliseconds(20);
        // Short bound: a poller woken by this step can block on the SDK lock a
        // wedged call holds, and would otherwise stall every step for the bound.
        clock.wait_for_woken_settled(milliseconds(2));
        std::this_thread::sleep_for(pace);
    }
    return spent;
}

// Steps virtual time in the background while it lives. Teardown on a failure
// path needs it: a failed REQUIRE unwinds into a driver destructor whose reap
// loops park on the fake clock, and nobody else would advance it.
class ClockPump {
public:
    explicit ClockPump(FakeTaskClock& clock)
        : thread_([this, &clock] {
              while (!stop_.load()) {
                  clock.advance(milliseconds(20));
                  std::this_thread::sleep_for(milliseconds(2));
              }
          }) {}
    ClockPump(const ClockPump&) = delete;
    ClockPump& operator=(const ClockPump&) = delete;
    ~ClockPump() {
        stop_ = true;
        thread_.join();
    }

private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// Declared after the driver (and any gate): on every exit path it opens the
// gate and destroys the driver while a ClockPump keeps virtual time moving.
struct TeardownGuard {
    FakeTaskClock& clock;
    std::unique_ptr<alpacacore::CameraDriver>& driver;
    SdkGate* gate = nullptr;
    ~TeardownGuard() {
        if (gate != nullptr) {
            gate->release = true;
        }
        ClockPump pump(clock);
        driver.reset();
    }
};

// Joins a helper thread on every exit path, opening the gate and pumping the
// clock first so the join cannot wait on a wedged call or a parked reap loop.
struct JoinPumped {
    FakeTaskClock& clock;
    std::thread& thread;
    SdkGate* gate = nullptr;
    ~JoinPumped() {
        if (gate != nullptr) {
            gate->release = true;
        }
        ClockPump pump(clock);
        if (thread.joinable()) {
            thread.join();
        }
    }
};

// Disconnects on a helper thread and steps virtual time until it returns. A
// clean disconnect needs one or two steps; the 10 ms of real time per step
// gives a worker that is still finishing about a second before the 2 s reap
// bound can detach it.
milliseconds disconnect_stepping(FakeTaskClock& clock, alpacacore::CameraDriver& driver) {
    std::atomic<bool> done{false};
    std::thread t([&] {
        driver.set_connected(false);
        done = true;
    });
    JoinPumped joiner{clock, t};
    return step_until(clock, [&] { return done.load(); }, milliseconds(10));
}

// Disconnects a driver with a wedged worker: steps virtual time until the
// reap logs `reaped`, then releases the gate (the wedged SDK call holds the
// SDK lock, so the rest of the disconnect cannot finish before that) and steps
// until the disconnect returns. Returns the virtual time up to the reap.
milliseconds disconnect_reaping(FakeTaskClock& clock, alpacacore::CameraDriver& driver, QhyLogCapture& logs,
                                SdkGate& gate, const std::string& reaped) {
    const auto waits_before = clock.wait_count();
    std::atomic<bool> done{false};
    std::thread t([&] {
        driver.set_connected(false);
        done = true;
    });
    JoinPumped joiner{clock, t, &gate};
    // The reap loop registers a new wait on the clock; the pollers being
    // stopped only exit. A loop on the real clock registers none.
    REQUIRE(clock.wait_for_wait_count(waits_before + 1, seconds(2)));
    const auto spent = step_until(clock, [&] { return !logs.find(reaped).empty(); });
    gate.release = true;
    step_until(clock, [&] { return done.load(); });
    return spent;
}

int logged_ms(const std::string& line) {
    const auto at = line.find("after ");
    return at == std::string::npos ? -1 : std::stoi(line.substr(at + 6));
}

}  // namespace

TEST_CASE("QHY Camera Driver - exposure limits fail fast during connect", "[qhy][camera][unit][getter-stall]") {
    const bool maximum = GENERATE(false, true);
    FakeTaskClock clock;
    SdkGate gate;
    auto fake = make_fake();
    gate.arm(fake, "init_camera");
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0, fake, clock);
    TeardownGuard teardown{clock, driver, &gate};
    driver->connect();
    REQUIRE(eventually([&] { return gate.entered.load(); }));
    std::atomic<int> error{0};
    std::thread reader([&] {
        try {
            if (maximum)
                (void)driver->get_exposure_max();
            else
                (void)driver->get_exposure_min();
            error = -1;
        } catch (const alpacacore::AlpacaException& ex) {
            error = ex.error_code();
        }
    });
    JoinPumped joiner{clock, reader, &gate};
    CHECK(eventually([&] { return error.load() != 0; }, milliseconds(500)));
    CHECK(error.load() == AlpacaError::NotConnected);
}

TEST_CASE("QHY Camera Driver - metadata is tied to worker arm not request or readout",
          "[qhy][camera][unit][taskclock][metadata]") {
    FakeTaskClock clock;
    SdkGate gate;
    auto fake = make_fake();
    gate.arm(fake, "register_exposure_worker");
    auto hold_registration = fake.before_call;
    std::thread::id arm_thread, download_thread;
    std::chrono::system_clock::time_point arm_upper;
    fake.before_call = [&](const std::string& name) {
        hold_registration(name);
        if (name == "start_single_frame") {
            arm_thread = std::this_thread::get_id();
            arm_upper = std::chrono::system_clock::now();
        } else if (name == "get_single_frame") {
            download_thread = std::this_thread::get_id();
        }
    };
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, clock);
    TeardownGuard guard{clock, driver, &gate};
    driver->set_connected(true);
    std::thread starter([&] { driver->start_exposure(0.012345, true); });
    JoinPumped joiner{clock, starter, &gate};
    REQUIRE(eventually([&] { return gate.entered.load(); }));
    alpacacore::test::require_last_exposure_unset(*driver);
    clock.advance(milliseconds(500));
    CHECK(driver->get_percent_completed() == 0.0);
    const auto arm_lower = std::chrono::system_clock::now();
    gate.release = true;
    starter.join();
    REQUIRE(clock.wait_for_waiters(1, seconds(2)));  // mandatory post-arm settle
    alpacacore::test::require_last_exposure_unset(*driver);
    CHECK_FALSE(driver->get_image_ready());
    clock.advance(milliseconds(6));
    ALPACA_REQUIRE_APPROX(driver->get_percent_completed(), 100.0 * 0.006 / 0.012345);
    clock.advance(milliseconds(994));
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));
    CHECK(driver->get_last_exposure_duration() == 0.012345);
    const auto start = driver->get_last_exposure_start_time();
    CHECK(start >= arm_lower);
    CHECK(start <= arm_upper);
    CHECK(arm_thread == download_thread);
    CHECK(arm_thread != std::this_thread::get_id());
    CHECK(disconnect_stepping(clock, *driver) < seconds(2));
}

TEST_CASE("QHY Camera Driver - pending cancelled and timed out frames cannot publish metadata",
          "[qhy][camera][unit][taskclock][metadata]") {
    FakeTaskClock clock;
    SdkGate gate;
    auto fake = make_fake();
    fake.read_directly = true;
    gate.arm(fake, "get_single_frame");
    gate.armed = false;
    auto hold_frame = fake.before_call;
    std::atomic<bool> cancelled{false};
    fake.before_call = [&](const std::string& name) {
        hold_frame(name);
        if (name == "cancel_exposure") cancelled = true;
    };
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, clock);
    TeardownGuard guard{clock, driver, &gate};
    const auto not_connected = [&] {
        CHECK(error_code_of([&] { (void)driver->get_last_exposure_duration(); }) == AlpacaError::NotConnected);
        CHECK(error_code_of([&] { (void)driver->get_last_exposure_start_time(); }) == AlpacaError::NotConnected);
    };
    not_connected();
    driver->set_connected(true);
    alpacacore::test::require_last_exposure_unset(*driver);
    const bool previous_success = GENERATE(false, true);
    std::chrono::system_clock::time_point previous_start{};
    if (previous_success) {
        driver->start_exposure(0.02, true);
        REQUIRE(eventually([&] { return driver->get_image_ready(); }));
        REQUIRE_NOTHROW(previous_start = driver->get_last_exposure_start_time());
        driver->set_connected(true);  // redundant connect keeps the pair
    }
    const auto unchanged = [&] {
        if (previous_success) {
            CHECK(driver->get_last_exposure_duration() == 0.02);
            CHECK(driver->get_last_exposure_start_time() == previous_start);
        } else {
            alpacacore::test::require_last_exposure_unset(*driver);
        }
    };
    gate.armed = true;
    driver->start_exposure(2.0, true);
    REQUIRE(eventually([&] { return gate.entered.load(); }));
    unchanged();
    CHECK_FALSE(driver->get_image_ready());
    CHECK(error_code_of([&] { driver->start_exposure(-1.0, true); }) == AlpacaError::InvalidValue);
    unchanged();
    SECTION("abort rejects even a late successful SDK frame") {
        cancelled = false;
        std::atomic<bool> abort_done{false};
        std::thread aborter([&] {
            driver->abort_exposure();
            abort_done = true;
        });
        JoinPumped joiner{clock, aborter, &gate};
        REQUIRE(eventually([&] { return cancelled.load(); }));
        gate.release = true;
        // A broken worker can report Idle before the aborter's clock-driven reap has finished.
        step_until(clock, [&] { return abort_done.load(); });
        REQUIRE(abort_done.load());
        aborter.join();
        CHECK_FALSE(driver->get_image_ready());
    }
    SECTION("watchdog rejects even a late successful SDK frame") {
        clock.advance(seconds(62));
        CHECK(error_code_of([&] { (void)driver->get_image_ready(); }) == AlpacaError::DriverException);
        gate.release = true;
    }
    SECTION("download failure preserves the pair") {
        fake.frame_ok = false;
        gate.release = true;
        REQUIRE(eventually(
            [&] { return error_code_of([&] { (void)driver->get_image_ready(); }) == AlpacaError::DriverException; }));
    }
    SECTION("a new success replaces both values") {
        gate.release = true;
        REQUIRE(eventually([&] { return driver->get_image_ready(); }));
        CHECK(driver->get_last_exposure_duration() == 2.0);
        CHECK(driver->get_last_exposure_start_time() >= previous_start);
        CHECK(disconnect_stepping(clock, *driver) < seconds(2));
        not_connected();
        driver->set_connected(true);
        alpacacore::test::require_last_exposure_unset(*driver);
        return;
    }
    unchanged();
    CHECK(disconnect_stepping(clock, *driver) < seconds(2));
    not_connected();
    driver->set_connected(true);
    alpacacore::test::require_last_exposure_unset(*driver);
}

namespace {

void check_failed_qhy_mode_configuration(bool fail_reinitialization) {
    // Model the real wrapper's mode selection followed by a potentially throwing InitQHYCCD.
    struct ModeSDK : FakeQHYSDK {
        ModeSDK() : FakeQHYSDK(FakeQHYSDK::with_one_camera()) {}
        void set_readout_mode(const std::string& id, uint32_t mode) override {
            FakeQHYSDK::set_readout_mode(id, mode);
            init_camera(id);
        }
    } fake;
    FakeTaskClock clock;
    fake.read_directly = true;
    fake.param_ranges[alpacacore::vendor::qhy::control::EXPOSURE].max = 5'000'000.0;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, clock);
    TeardownGuard guard{clock, driver};
    driver->set_connected(true);
    REQUIRE(driver->get_exposure_max() == 5.0);
    fake.param_ranges[alpacacore::vendor::qhy::control::EXPOSURE].max = 3'000'000.0;
    const bool range_returns_failure = !fail_reinitialization && GENERATE(false, true);
    if (range_returns_failure) {
        fake.param_ranges[alpacacore::vendor::qhy::control::EXPOSURE].available = false;
    } else {
        fake.throw_from.insert(fail_reinitialization ? "init_camera" : "get_param_range");
    }
    CHECK(error_code_of([&] { driver->set_readout_mode(1); }) == AlpacaError::DriverException);
    CHECK(fake.call_count("set_readout_mode") == 1);
    CHECK(fake.call_count("init_camera") == 2);
    CHECK(fake.call_count("get_param_range") == (fail_reinitialization ? 1 : 2));
    CHECK(driver->get_connected());
    const int cancellations = fake.call_count("cancel_exposure");
    const int arms = fake.call_count("start_single_frame");
    CHECK(error_code_of([&] { (void)driver->get_exposure_max(); }) == AlpacaError::DriverException);
    CHECK(error_code_of([&] { (void)driver->get_exposure_min(); }) == AlpacaError::DriverException);
    CHECK(error_code_of([&] { driver->start_exposure(1.0, true); }) == AlpacaError::DriverException);
    CHECK(error_code_of([&] { driver->start_exposure(-1.0, true); }) == AlpacaError::InvalidValue);
    CHECK(error_code_of([&] { driver->set_readout_mode(-1); }) == AlpacaError::InvalidValue);
    CHECK(fake.call_count("cancel_exposure") == cancellations);
    CHECK(fake.call_count("start_single_frame") == arms);
    alpacacore::test::require_last_exposure_unset(*driver);
    // Removing injection alone must not make the old cached limits authoritative again.
    fake.throw_from.clear();
    fake.param_ranges[alpacacore::vendor::qhy::control::EXPOSURE].available = true;
    CHECK(error_code_of([&] { (void)driver->get_exposure_max(); }) == AlpacaError::DriverException);
    if (GENERATE(false, true)) {
        CHECK(disconnect_stepping(clock, *driver) < seconds(2));
        driver->set_connected(true);
    } else {
        driver->set_readout_mode(1);
    }
    CHECK(driver->get_exposure_max() == 3.0);
    CHECK(driver->get_exposure_min() == 0.0001);
    driver->start_exposure(1.0, true);
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));
    CHECK(driver->get_last_exposure_duration() == 1.0);
    CHECK(disconnect_stepping(clock, *driver) < seconds(2));
}

}  // namespace

TEST_CASE("QHY Camera Driver - failed reinitialization after mode selection invalidates exposure limits",
          "[qhy][camera][unit][taskclock][metadata]") {
    check_failed_qhy_mode_configuration(true);
}

TEST_CASE("QHY Camera Driver - failed range refresh invalidates exposure limits",
          "[qhy][camera][unit][taskclock][metadata]") {
    check_failed_qhy_mode_configuration(false);
}

TEST_CASE("QHY Camera Driver - above maximum exposure preserves the acquisition in flight",
          "[qhy][camera][unit][taskclock][metadata]") {
    FakeTaskClock clock;
    SdkGate gate;
    auto fake = make_fake();
    fake.read_directly = true;
    fake.param_ranges[alpacacore::vendor::qhy::control::EXPOSURE].max = 5'000'000.0;
    gate.arm(fake, "get_single_frame");
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, clock);
    TeardownGuard guard{clock, driver, &gate};
    driver->set_connected(true);
    const bool changed_mode = GENERATE(false, true);
    if (changed_mode) {
        fake.param_ranges[alpacacore::vendor::qhy::control::EXPOSURE].max = 3'000'000.0;
        driver->set_readout_mode(1);
    }
    const double maximum = driver->get_exposure_max();
    CHECK(maximum == (changed_mode ? 3.0 : 5.0));
    for (double duration : {-1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        require_alpaca_error([&] { driver->start_exposure(duration, true); }, AlpacaError::InvalidValue);
    }
    driver->start_exposure(2.0, true);
    REQUIRE(eventually([&] { return gate.entered.load(); }));
    const int cancellations = fake.call_count("cancel_exposure");
    const int range_reads = fake.call_count("get_param_range");
    std::atomic<bool> done{false};
    int error = 0;
    std::thread rejecter([&] {
        error = error_code_of([&] { driver->start_exposure(maximum + 1.0, true); });
        done = true;
    });
    JoinPumped joiner{clock, rejecter, &gate};
    REQUIRE(eventually([&] { return done.load(); }));  // must not wait behind the held SDK download
    rejecter.join();
    CHECK(error == AlpacaError::InvalidValue);
    CHECK(fake.call_count("cancel_exposure") == cancellations);
    CHECK(fake.call_count("get_param_range") == range_reads);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    gate.release = true;
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));
    CHECK(driver->get_last_exposure_duration() == 2.0);
    CHECK(disconnect_stepping(clock, *driver) < seconds(2));
}

TEST_CASE("QHY Camera Driver - failed replacement setup retires the cancelled acquisition",
          "[qhy][camera][unit][taskclock][metadata]") {
    const auto failing_call =
        GENERATE("set_param", "is_control_available", "set_resolution", "set_bin_mode", "set_bits_mode");
    const bool previous_success = GENERATE(false, true);
    CAPTURE(failing_call, previous_success);
    FakeTaskClock clock;
    SdkGate gate;
    auto fake = make_fake();
    fake.read_directly = true;
    gate.arm(fake, "get_single_frame");
    gate.armed = false;
    const auto hold_frame = fake.before_call;
    std::atomic<bool> cancel_releases_frame{false};
    fake.before_call = [&](const std::string& name) {
        if (name == "cancel_exposure" && cancel_releases_frame.load()) gate.release = true;
        hold_frame(name);
    };
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, clock);
    TeardownGuard guard{clock, driver, &gate};
    driver->set_connected(true);
    std::chrono::system_clock::time_point previous_start{};
    if (previous_success) {
        driver->start_exposure(0.02, true);
        REQUIRE(eventually([&] { return driver->get_image_ready(); }));
        previous_start = driver->get_last_exposure_start_time();
    }
    gate.armed = true;
    gate.release = false;
    {
        ClockPump pump(clock);
        driver->start_exposure(3000.0, true);
    }
    REQUIRE(eventually([&] { return gate.entered.load(); }));
    fake.throw_from.insert(failing_call);
    cancel_releases_frame = true;
    {
        ClockPump pump(clock);
        CHECK(error_code_of([&] { driver->start_exposure(1.0, false); }) == AlpacaError::DriverException);
    }
    fake.throw_from.clear();
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK_FALSE(driver->get_image_ready());
    CHECK(error_code_of([&] { (void)driver->get_image_array(); }) == AlpacaError::InvalidOperation);
    CHECK_NOTHROW(driver->set_gain(12));
    clock.advance(seconds(3601));
    CHECK_FALSE(driver->get_image_ready());  // The cancelled deadline must not expire later.
    if (previous_success) {
        CHECK(driver->get_last_exposure_duration() == 0.02);
        CHECK(driver->get_last_exposure_start_time() == previous_start);
    } else {
        alpacacore::test::require_last_exposure_unset(*driver);
    }
    gate.armed = false;
    driver->start_exposure(1.0, true);
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));
    CHECK(driver->get_last_exposure_duration() == 1.0);
}

TEST_CASE("QHY Camera Driver - an unpolled deadline rejects a late successful frame",
          "[qhy][camera][unit][taskclock][metadata]") {
    // Observe worker completion without calling a getter that could expire the deadline itself.
    struct ObservedSDK : LockedQHYSDK {
        using LockedQHYSDK::LockedQHYSDK;
        std::shared_ptr<std::atomic<bool>> running;
        void register_exposure_worker(const std::string& id, std::shared_ptr<std::atomic<bool>> flag) override {
            running = flag;
            LockedQHYSDK::register_exposure_worker(id, std::move(flag));
        }
    };
    FakeTaskClock clock;
    SdkGate gate;
    auto fake = make_fake();
    fake.read_directly = true;
    gate.arm(fake, "get_single_frame");
    gate.armed = false;
    ObservedSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, clock);
    TeardownGuard guard{clock, driver, &gate};
    driver->set_connected(true);
    const bool previous_success = GENERATE(false, true);
    std::chrono::system_clock::time_point previous_start{};
    if (previous_success) {
        driver->start_exposure(0.02, true);
        REQUIRE(eventually([&] { return !sdk.running->load(); }));
        REQUIRE(driver->get_image_ready());
        previous_start = driver->get_last_exposure_start_time();
    }
    gate.armed = true;
    driver->start_exposure(2.0, true);
    REQUIRE(eventually([&] { return gate.entered.load(); }));
    clock.advance(seconds(63));  // exposure + default 60s margin, with no watchdog getter
    gate.release = true;
    REQUIRE(eventually([&] { return !sdk.running->load(); }));
    if (previous_success) {
        CHECK(driver->get_last_exposure_duration() == 0.02);
        CHECK(driver->get_last_exposure_start_time() == previous_start);
    } else {
        alpacacore::test::require_last_exposure_unset(*driver);
    }
    CHECK(error_code_of([&] { (void)driver->get_image_ready(); }) == AlpacaError::DriverException);
    CHECK(error_code_of([&] { (void)driver->get_image_array(); }) == AlpacaError::DriverException);
    CHECK(disconnect_stepping(clock, *driver) < seconds(2));
}

TEST_CASE("QHY Camera Driver - the exposure watchdog expires on the injected clock", "[qhy][camera][unit][taskclock]") {
    // Falsified by: qhy_camera_driver.cpp start_exposure() stamping
    // exposure_deadline_ from steady_clock::now() (virtual time then never
    // reaches the deadline) or expire_overdue_exposure_locked() comparing
    // against steady_clock::now().
    FakeTaskClock clock;
    SdkGate gate;
    auto fake = make_fake();
    fake.read_directly = true;
    gate.arm(fake, "get_single_frame");
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, {}, seconds(1), clock);
    TeardownGuard guard{clock, driver, &gate};
    driver->set_connected(true);
    driver->start_exposure(2.0, true);
    REQUIRE(eventually([&] { return gate.entered.load(); }));

    // Deadline = 2 s exposure + 1 s margin on the fake clock.
    clock.advance(milliseconds(2999));
    CHECK_FALSE(driver->get_image_ready());
    clock.advance(milliseconds(1));
    CHECK(error_code_of([&] { (void)driver->get_image_ready(); }) == alpacacore::AlpacaError::DriverException);

    gate.release = true;
    CHECK(disconnect_stepping(clock, *driver) < seconds(2));
}

TEST_CASE("QHY Camera Driver - a download in a slow readout mode extends the watchdog on the injected clock",
          "[qhy][camera][unit][taskclock]") {
    // Falsified by: qhy_camera_driver.cpp exposure worker computing
    // extended_deadline from steady_clock::now() (the extension would be
    // measured from real time, so the deadline stays at 2 s virtual).
    FakeTaskClock clock;
    SdkGate gate;
    auto fake = make_fake();
    fake.read_directly = true;
    fake.mem_length_override = 500'000;  // 1 s of transfer at the 500 kB/s floor
    gate.arm(fake, "get_single_frame");
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, {}, seconds(1), clock);
    TeardownGuard guard{clock, driver, &gate};
    driver->set_connected(true);
    driver->set_readout_mode(1);
    driver->start_exposure(1.0, true);
    REQUIRE(eventually([&] { return gate.entered.load(); }));

    // Base deadline 1 s + 1 s margin = 2 s; extended = 1 s transfer + 15 s = 16 s.
    clock.advance(seconds(15));
    CHECK_FALSE(driver->get_image_ready());
    clock.advance(seconds(1));
    CHECK(error_code_of([&] { (void)driver->get_image_ready(); }) == alpacacore::AlpacaError::DriverException);

    gate.release = true;
    CHECK(disconnect_stepping(clock, *driver) < seconds(2));
}

TEST_CASE("QHY Camera Driver - the pre-download settle sleep waits on the injected clock",
          "[qhy][camera][unit][taskclock]") {
    // Falsified by: qhy_camera_driver.cpp exposure worker sleeping with
    // this_thread::sleep_for(1 s) (no waiter registers on the fake clock).
    FakeTaskClock clock;
    auto fake = make_fake();  // read_directly false: the worker settles for 1 s first
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, {}, seconds(60), clock);
    TeardownGuard guard{clock, driver};
    driver->set_connected(true);
    driver->start_exposure(0.05, true);
    REQUIRE(clock.wait_for_waiters(1, seconds(2)));
    CHECK(fake.call_count("get_single_frame") == 0);

    clock.advance(milliseconds(999));
    CHECK(fake.call_count("get_single_frame") == 0);
    clock.advance(milliseconds(1));
    CHECK(eventually([&] { return driver->get_image_ready(); }));
    CHECK(fake.call_count("get_single_frame") == 1);

    CHECK(disconnect_stepping(clock, *driver) < seconds(2));
}

TEST_CASE("QHY Camera Driver - a pulse guide ends on the injected clock", "[qhy][camera][unit][taskclock]") {
    // Falsified by: qhy_camera_driver.cpp pulse_guide() stamping
    // pulse_guiding_end_ or get_is_pulse_guiding() reading steady_clock::now().
    FakeTaskClock clock;
    SdkGate gate;
    auto fake = make_fake();
    // The real guide call blocks for the pulse; hold the fake's the same way, so
    // the worker leaves the flag set and only the end time can clear it.
    gate.arm(fake, "guide");
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, clock);
    TeardownGuard guard{clock, driver, &gate};
    driver->set_connected(true);
    REQUIRE(driver->get_can_pulse_guide());

    driver->pulse_guide(0, 500);
    REQUIRE(eventually([&] { return gate.entered.load(); }));
    CHECK(driver->get_is_pulse_guiding());
    clock.advance(milliseconds(499));
    CHECK(driver->get_is_pulse_guiding());
    clock.advance(milliseconds(1));
    CHECK_FALSE(driver->get_is_pulse_guiding());

    gate.release = true;
    CHECK(disconnect_stepping(clock, *driver) < seconds(2));
}

TEST_CASE("QHY Camera Driver - a wedged download is reaped after 2 s of injected time",
          "[qhy][camera][unit][taskclock]") {
    // Falsified by: qhy_camera_driver.cpp join_exposure_thread() reading
    // steady_clock::now() or sleeping with this_thread::sleep_for (the
    // disconnect then returns after 2 real seconds, or never on the fake).
    FakeTaskClock clock;
    QhyLogCapture logs;
    SdkGate gate;
    auto fake = make_fake();
    fake.read_directly = true;
    gate.arm(fake, "get_single_frame");
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, clock);
    TeardownGuard guard{clock, driver, &gate};
    driver->set_connected(true);
    driver->start_exposure(0.05, true);
    REQUIRE(eventually([&] { return gate.entered.load(); }));

    std::atomic<bool> done{false};
    std::thread t([&] {
        driver->set_connected(false);
        done = true;
    });
    JoinPumped joiner{clock, t, &gate};
    REQUIRE(clock.wait_for_waiters(1, seconds(2)));
    clock.advance(milliseconds(1980));
    clock.wait_for_woken_settled(milliseconds(500));
    CHECK(logs.find("exceeded 2s reap timeout").empty());  // 1980 ms of a 2000 ms bound
    const auto spent = step_until(clock, [&] { return !logs.find("exceeded 2s reap timeout").empty(); });
    CHECK(spent <= milliseconds(40));
    gate.release = true;  // the wedged call holds the SDK lock the rest of the disconnect needs
    step_until(clock, [&] { return done.load(); });
}

TEST_CASE("QHY Camera Driver - a wedged cooler-off worker is reaped after 2 s of injected time and timed on it",
          "[qhy][camera][unit][taskclock]") {
    // Falsified by: qhy_camera_driver.cpp join_cooler_off_thread() reading
    // steady_clock::now(), or the cooler-off worker timing its SetParam call
    // with steady_clock::now() (the logged duration is then real milliseconds).
    FakeTaskClock clock;
    QhyLogCapture logs;
    SdkGate gate;
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    gate.arm(fake, "set_param");
    gate.armed = false;  // the connect also calls set_param; only the worker's call is held
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, clock);
    TeardownGuard guard{clock, driver, &gate};
    driver->set_connected(true);
    gate.armed = true;
    driver->set_cooler_on(false);
    // The worker first reaps the temperature thread, which may park in a 20 ms
    // sleep on the clock; step until it reaches the held SetParam call.
    step_until(clock, [&] { return gate.entered.load(); });
    REQUIRE(gate.entered.load());

    const auto spent = disconnect_reaping(clock, *driver, logs, gate, "cooler-off worker exceeded 2s timeout");
    CHECK(spent >= milliseconds(2000));
    CHECK(spent < milliseconds(2500));

    REQUIRE(eventually([&] { return !logs.find("SetQHYCCDParam(MANULPWM,0) returned OK after").empty(); }));
    // The call began before the 2 s reap and returned after it, on virtual time.
    CHECK(logged_ms(logs.find("returned OK after")) >= 2000);
}

TEST_CASE("QHY Camera Driver - a wedged temperature worker is reaped after 2 s of injected time",
          "[qhy][camera][unit][taskclock]") {
    // Falsified by: qhy_camera_driver.cpp join_worker_thread() reading
    // steady_clock::now() or sleeping with this_thread::sleep_for.
    FakeTaskClock clock;
    QhyLogCapture logs;
    SdkGate gate;
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    gate.arm(fake, "control_temp");
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, clock);
    TeardownGuard guard{clock, driver, &gate};
    driver->set_connected(true);
    REQUIRE(eventually([&] { return gate.entered.load(); }));

    const auto spent = disconnect_reaping(clock, *driver, logs, gate, "temp-control thread exceeded 2s timeout");
    CHECK(spent >= milliseconds(2000));
    CHECK(spent < milliseconds(2500));
}

TEST_CASE("QHY Camera Driver - the polling workers wait a second on the injected clock",
          "[qhy][camera][unit][taskclock]") {
    // Falsified by: qhy_camera_driver.cpp WorkerStopSignal::wait_for() using
    // condition_variable::wait_for (no waiter registers, and the workers
    // poll on real time instead of the advance below).
    FakeTaskClock clock;
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, clock);
    TeardownGuard guard{clock, driver};
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(2, seconds(2)));  // temperature + telemetry
    const int temp_polls = fake.call_count("control_temp");
    const int telemetry_polls = fake.call_count("get_param");
    REQUIRE(temp_polls >= 1);

    clock.advance(milliseconds(999));
    CHECK(fake.call_count("control_temp") == temp_polls);
    CHECK(fake.call_count("get_param") == telemetry_polls);
    clock.advance(milliseconds(1));
    CHECK(eventually([&] { return fake.call_count("control_temp") > temp_polls; }));
    CHECK(eventually([&] { return fake.call_count("get_param") > telemetry_polls; }));

    CHECK(disconnect_stepping(clock, *driver) < seconds(2));
}

TEST_CASE("QHY Camera Driver - the polling workers skip a cycle during an exposure on the injected clock",
          "[qhy][camera][unit][taskclock]") {
    // Falsified by: qhy_camera_driver.cpp telemetry worker's exposing-branch
    // wait using condition_variable::wait_for (only one waiter registers
    // after the advance below).
    FakeTaskClock clock;
    SdkGate gate;
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    fake.read_directly = true;
    gate.arm(fake, "get_single_frame");
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk, {}, seconds(600), clock);
    TeardownGuard guard{clock, driver, &gate};
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(2, seconds(2)));
    driver->start_exposure(5.0, true);
    REQUIRE(eventually([&] { return gate.entered.load(); }));
    const int temp_polls = fake.call_count("control_temp");
    const int telemetry_polls = fake.call_count("get_param");

    clock.advance(seconds(1));
    CHECK(clock.wait_for_woken_settled(seconds(2)));
    CHECK(clock.wait_for_waiters(2, seconds(2)));  // both back in a wait, neither polled
    CHECK(fake.call_count("control_temp") == temp_polls);
    CHECK(fake.call_count("get_param") == telemetry_polls);

    gate.release = true;
    CHECK(disconnect_stepping(clock, *driver) < seconds(2));
}

// Issue #1477: ICameraV4 PercentCompleted raises InvalidOperation outside Exposing; CoolerOn raises
// PropertyNotImplemented on a camera without a cooler (CoolerPower always does: CanGetCoolerPower is false).
TEST_CASE("QHY Camera Driver - PercentCompleted only while Exposing", "[qhy][camera][unit]") {
    std::atomic<bool> hold_frame{false};
    std::atomic<bool> in_frame{false};
    std::atomic<bool> release{false};
    auto fake = FakeQHYSDK::with_one_camera();
    fake.before_call = [&](const std::string& fn) {
        if (hold_frame.load() && fn == "get_single_frame") {
            in_frame = true;
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    // Declared after the driver so a failed REQUIRE releases the held frame before the driver joins its worker.
    struct ReleaseOnExit {
        std::atomic<bool>& flag;
        ~ReleaseOnExit() { flag = true; }
    } release_on_exit{release};
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    SECTION("idle before the first exposure") { alpacacore::test::require_percent_completed_unavailable(*driver); }
    SECTION("idle after a completed exposure") {
        driver->start_exposure(0.001, true);
        REQUIRE(eventually([&]() { return driver->get_image_ready(); }));
        alpacacore::test::require_percent_completed_unavailable(*driver);
    }
    SECTION("idle after a failed exposure") {
        fake.frame_ok = false;
        driver->start_exposure(0.001, true);
        REQUIRE(eventually([&]() {
            try {
                (void)driver->get_image_ready();
                return false;
            } catch (const alpacacore::AlpacaException&) {
                return true;  // the failed frame is reported, not served
            }
        }));
        alpacacore::test::require_percent_completed_unavailable(*driver);
    }
    SECTION("while exposing") {
        hold_frame = true;
        driver->start_exposure(1.0, true);
        REQUIRE(eventually([&]() { return in_frame.load(); }));
        alpacacore::test::require_percent_completed_available(*driver);
    }
    release = true;
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - CoolerOn needs the cooler capability", "[qhy][camera][unit]") {
    auto fake = FakeQHYSDK::with_one_camera();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK_FALSE(driver->get_can_set_ccd_temperature());
    alpacacore::test::require_no_cooler_members(*driver);
    driver->set_connected(false);
}

TEST_CASE("QHY Camera Driver - CoolerOn works with the cooler capability", "[qhy][camera][unit]") {
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_can_set_ccd_temperature());
    driver->set_cooler_on(true);
    CHECK(driver->get_cooler_on());
    require_alpaca_error([&] { (void)driver->get_cooler_power(); }, alpacacore::AlpacaError::PropertyNotImplemented);
    driver->set_connected(false);
}

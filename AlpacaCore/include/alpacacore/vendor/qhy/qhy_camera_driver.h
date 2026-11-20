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

#pragma once

#include <alpacacore/camera_driver.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace alpacacore::vendor::qhy {

/**
 * @brief Create a QHY camera driver by string camera ID.
 *
 * The camera ID is the unique identifier returned by the QHY SDK
 * (e.g., "QHY600-Pro-M-c7b72b").
 *
 * @param device_number Alpaca device number
 * @param camera_id QHY SDK camera ID string
 * @return Unique pointer to camera driver
 */
std::unique_ptr<CameraDriver> create_qhy_camera(int device_number, const std::string& camera_id);

/**
 * @brief Create a QHY camera driver by camera index (enumeration order).
 *
 * @param device_number Alpaca device number
 * @param camera_index QHY SDK camera index (0-based)
 * @return Unique pointer to camera driver
 */
std::unique_ptr<CameraDriver> create_qhy_camera_by_index(int device_number, int camera_index);

/**
 * @brief Test overloads taking an explicit SDK seam (issue #321).
 *
 * The default overloads above pass QHYSDKWrapper::instance(). These let a test
 * inject a FakeQHYSDK instead — the only way to reach this driver's connect
 * path, since the real SDK cannot initialise on a USB-less host.
 *
 * Every exposure, pulse-guide and reap deadline and every worker wait reads the
 * `clock` argument (decision 0005); a test passes a FakeTaskClock and advance()s
 * instead of sleeping. The clock MUST outlive the returned driver, like the SDK
 * reference. Pointing time (UTCDate, last
 * exposure start) stays on the system clock.
 *
 * The SDK reference MUST outlive the returned driver. The exposure body runs in a
 * util::CaptureSession and the temperature, telemetry, cooler-off and pulse-guide
 * bodies in util::AsyncOperation slots; the driver joins them all (issue #1736), so
 * a disconnect or the destructor waits for a call stuck inside the SDK. See the member comment in qhy_camera_driver.cpp
 * and rule (b) in testing.instructions.md ("Hardware-free driver tests via the SDK seam").
 */
std::unique_ptr<CameraDriver> create_qhy_camera(int device_number, const std::string& camera_id, QHYSDK& sdk,
                                                util::TaskClock& clock = util::default_task_clock());
std::unique_ptr<CameraDriver> create_qhy_camera_by_index(int device_number, int camera_index, QHYSDK& sdk,
                                                         util::TaskClock& clock = util::default_task_clock());

/// The camera driver's workers, as named to a QHYWorkerStartHook.
enum class QHYWorker : std::uint8_t { Telemetry, TempControl, CoolerOff };

/**
 * @brief Test-only hook, called by each polling worker start after its checks
 * and before its slot starts the body, with the start mutex held (issues #510, #1736).
 *
 * A disconnect that landed between building and storing a worker used to
 * strand it for the life of the driver, and no SDK call runs there, so
 * FakeQHYSDK::before_call cannot hold it open. The hook runs on the starting
 * thread with `slot_start_mutex_` held (no other driver lock); it must not call
 * back into the driver's starters (CoolerOn=true, Connected=true), and must not throw. A hook that blocks
 * must be released before the driver is destroyed. Production passes none.
 * CoolerOff instead calls the hook on its worker thread before taking the
 * state mutex; a throwing hook is caught and ends that test-only worker.
 */
using QHYWorkerStartHook = std::function<void(QHYWorker)>;

std::unique_ptr<CameraDriver> create_qhy_camera(int device_number, const std::string& camera_id, QHYSDK& sdk,
                                                QHYWorkerStartHook on_worker_start,
                                                std::chrono::seconds watchdog_margin = std::chrono::seconds(60),
                                                util::TaskClock& clock = util::default_task_clock());

/**
 * @brief Test-only seam (#2431): runs `hook` inside a disconnect, after it has stopped the exposure (or ended a
 * start still reserved) and released the state lock, and before it closes the capture session and joins the body.
 *
 * No SDK call runs there, so FakeQHYSDK::before_call cannot hold it open. The hook runs on the disconnecting thread
 * with only `transition_mutex_` held, so a StartExposure (which never takes that lock) can finish in the window. It
 * must not call back into the driver's connect or disconnect, and a hook that blocks must be released before the
 * driver is destroyed. Empty by default; an empty `hook` clears it. A driver that is not a QHY camera is ignored.
 */
void set_disconnect_stop_hook_for_testing(CameraDriver& driver, std::function<void()> hook);

} // namespace alpacacore::vendor::qhy

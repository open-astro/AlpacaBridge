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
#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>

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
 * The SDK reference MUST outlive the returned driver, INCLUDING this driver's
 * detachable workers: the exposure, temperature, cooler-off AND telemetry
 * threads join with a bounded timeout and detach on expiry, and the
 * pulse-guide thread is detached by design. Telemetry joined that list in
 * issue #323: its join used to be unbounded (a poll wedged inside an SDK call
 * hung the disconnect forever), so it now goes through the same
 * join_worker_thread() helper as the temperature worker, and the capture
 * below is what makes its SDK access survivable once detached.
 *
 * All five workers reach the SDK through a captured QHYSDK* rather than
 * through the driver's `sdk_` member.
 *
 * That capture NARROWS the use-after-free window; it does NOT close it. Every
 * one of those workers except pulse-guide also captures `this` and
 * dereferences it after the SDK call returns (`connected_`, `mutex_`, the
 * `exposure_superseded` re-check), which is UB if the driver is already gone.
 * Do not read this as "a worker that only touches the seam is safe to
 * detach": these workers are unsafe once detached, and the bounded-join
 * discipline is what keeps that window small. See the member comment in
 * qhy_camera_driver.cpp and rule (b) in AGENTS.md.
 */
std::unique_ptr<CameraDriver> create_qhy_camera(int device_number, const std::string& camera_id, QHYSDK& sdk);
std::unique_ptr<CameraDriver> create_qhy_camera_by_index(int device_number, int camera_index, QHYSDK& sdk);

} // namespace alpacacore::vendor::qhy

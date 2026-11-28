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

// The remotealpaca Camera proxied device (decision 0023, issue #1753 slice 5, #2703): the Camera device of an upstream
// Alpaca server, served as a device of this server with this server's own device number and UniqueID. The
// configuration, self-loop, Connect, snapshot, link-loss and pass-through rules are the ObservingConditions device's
// (remotealpaca_observingconditions_driver.h). CameraState, ImageReady, PercentCompleted, the temperatures,
// CoolerPower, IsPulseGuiding and LastExposure* are served from the polled frame; the sensor's description and the
// client's settings are read at Connect; StartExposure, the setters, PulseGuide, AbortExposure and StopExposure are
// forwarded in the command slot, and AbortExposure and StopExposure are forwarded even while the link is faulted. The
// image is fetched from the upstream's imagearray, as ImageBytes, in the command path.

#include <alpacacore/camera_driver.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_device_config.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>

namespace alpacacore::vendor::remotealpaca {

// The image download's bounds (issue #2703). The body cap is the sensor's full frame (CameraXSize x CameraYSize, three
// planes for a Color sensor) at four bytes an element, the widest integer type this server reads, plus the metadata
// and room for an error text; never more than kImageBodyCeiling, whatever size an upstream claims, and never less
// than util::kHttpMaxResponseBytes, so a JSON error reply always fits. The timeout is the per-call one plus the cap at
// kImageTransferFloor (a slow Wi-Fi link moves several times that), at most kImageFetchTimeoutCeiling: the 5 s
// per-call timeout would end any real frame's download.
inline constexpr std::size_t kImageBodyCeiling = std::size_t{512} * 1024 * 1024;
inline constexpr std::size_t kImageTransferFloor = std::size_t{1024} * 1024;  // bytes per second
inline constexpr std::chrono::seconds kImageFetchTimeoutCeiling{300};

struct ImageFetchBounds {
    std::size_t max_body_bytes = 0;
    std::chrono::milliseconds timeout{0};
};

// The bounds for a sensor of `width` by `height` pixels, `planes` (1, or 3 for a Color sensor) each; a size of 0 or
// less (the upstream did not answer it) takes the ceiling.
ImageFetchBounds image_fetch_bounds(int width, int height, int planes);

// Throws AlpacaException(InvalidValue) for a malformed host, a port outside 1 to 65535, a negative upstream device
// number or a poll period outside 1 to 60 seconds. Touches no network. `self` answers this server's listening
// addresses at each Connect (decision 0023 rule 4); empty means no check.
std::unique_ptr<CameraDriver> create_remotealpaca_camera(int device_number, const RemoteDeviceConfig& config,
                                                         ServerAddressSource self = {});

/**
 * @brief As above, calling the upstream through `http` and polling on `clock` (test seams, decisions 0015 and 0005).
 *
 * The production overload passes util::make_curl_http_client() and the real task clock. `http` is called from the
 * connect thread, the poll and the command bodies; the image download runs beside the other calls (the client's rule
 * 6), every other call one at a time per driver. The clock must outlive the driver.
 */
std::unique_ptr<CameraDriver> create_remotealpaca_camera(int device_number, const RemoteDeviceConfig& config,
                                                         std::shared_ptr<util::HttpClient> http,
                                                         ServerAddressSource self, util::TaskClock& clock);

// Test seam: while paused the poll keeps its period and samples nothing, so the frame ages without a failed poll
// (the contract sweep's stale-frame case). No effect on a driver of another type.
void set_poll_paused_for_testing(CameraDriver& driver, bool paused);

// Runs `hook` first inside each command body (StartExposure, the setters, PulseGuide, AbortExposure, StopExposure and
// the image download), before its call, so a test can hold a body before anything reaches the upstream. Set it while
// no command runs.
void set_command_hook_for_testing(CameraDriver& driver, std::function<void()> hook);

// Runs `hook` in a command's caller that finds an earlier command still running, before it waits for it (commands
// reach the upstream in caller order), so a test knows the caller is waiting. Set it while no command runs.
void set_command_waiting_hook_for_testing(CameraDriver& driver, std::function<void()> hook);

}  // namespace alpacacore::vendor::remotealpaca

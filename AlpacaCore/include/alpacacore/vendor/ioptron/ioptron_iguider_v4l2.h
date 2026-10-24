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

// The iOptron iGuider is a plain UVC camera (USB 0130:1466, "iOptron iGuider
// External"), driven through the kernel's uvcvideo driver and V4L2, with no
// vendor SDK. This is the seam the driver reaches it through (decision 0015):
// IGuiderV4L2Wrapper is the production implementation over the V4L2 ioctls,
// and the tests inject FakeIGuiderV4L2. Protocol notes and sources:
// AlpacaCore/external/iOptron/iGuider-protocol.md.

#include <alpacacore/util/error_handling.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace alpacacore::vendor::ioptron {

/// The UVC controls the driver writes. Values are the raw V4L2 control values.
enum class IGuiderControl : std::uint8_t {
    /// V4L2_CID_EXPOSURE_AUTO: 1 = manual (V4L2_EXPOSURE_MANUAL).
    AutoExposure,
    /// V4L2_CID_EXPOSURE_ABSOLUTE: an iGuider tick code, not 100 us units.
    ExposureAbsolute,
    /// V4L2_CID_GAIN: 64..480, step 32.
    Gain,
};

/// One V4L2 capture node whose card name says it is an iGuider.
struct IGuiderDeviceInfo {
    std::string path;  // e.g. /dev/video0
    std::string card;  // V4L2 card name, e.g. "iOptron iGuider External: iOptr"
    std::string bus_info;
};

/**
 * A seam call failed because the camera is gone: EIO, ENXIO, ENODEV or EBADF
 * on its fd (decision 0009, point 2a). Still an AlpacaException with the
 * DriverException number, so a caller that does not know about it sees the
 * failure it saw before; the stream worker catches it to drop the link.
 */
class IGuiderDeviceGone : public AlpacaException {
public:
    using AlpacaException::AlpacaException;
};

/**
 * The V4L2 operations the iGuider driver needs, by handle.
 *
 * Calls on one handle come from one thread at a time: connect runs them before
 * the stream worker starts, the worker owns them while it runs, and close comes
 * after the worker is joined. The production wrapper still serialises each
 * handle's calls itself (decision 0015).
 *
 * Every failure throws AlpacaException(DriverException) with the errno text,
 * as IGuiderDeviceGone when the errno says the camera is gone.
 */
class IGuiderV4L2 {
public:
    IGuiderV4L2(const IGuiderV4L2&) = delete;
    IGuiderV4L2& operator=(const IGuiderV4L2&) = delete;

    /// Capture nodes whose card name contains "iGuider", in /dev/video order.
    virtual std::vector<IGuiderDeviceInfo> enumerate() = 0;

    /// Opens a capture node and checks it can capture and stream and that its
    /// card name says iGuider (a pinned node another camera moved onto is refused).
    /// Returns a handle.
    virtual int open(const std::string& path) = 0;

    /// Stops any stream and closes the handle. Unknown handles are ignored.
    virtual void close(int handle) = 0;

    virtual void set_control(int handle, IGuiderControl control, std::int32_t value) = 0;

    /// Sets YUYV at width x height, maps the buffers and starts streaming.
    virtual void start_stream(int handle, int width, int height) = 0;

    /// Stops streaming and releases the buffers. Harmless when not streaming.
    virtual void stop_stream(int handle) = 0;

    /**
     * Waits up to @p timeout for the next complete frame and copies its luma
     * plane (every even YUYV byte, width x height bytes) into @p luma.
     * Returns false when no complete frame arrived in time; a short or
     * error-flagged frame is dropped and counts as none.
     * No photon-start timestamp is promised: uvcvideo sets MONOTONIC | SOE
     * even when its timestamp is host packet arrival (Linux 6.12 uvc_queue.c /
     * uvc_video.c). EOF is arrival of the last pixel, UNKNOWN has no known
     * clock domain, and sequence counts frames, not control application.
     * Exposing UTC start requires verified device timing, not dequeue minus duration.
     */
    virtual bool read_frame(int handle, std::vector<std::uint8_t>& luma, std::chrono::milliseconds timeout) = 0;

protected:
    IGuiderV4L2() = default;
    // Nothing owns a seam pointer (drivers hold a reference), so delete through
    // the interface must not compile (decision 0015, point 3).
    ~IGuiderV4L2() = default;
};

}  // namespace alpacacore::vendor::ioptron

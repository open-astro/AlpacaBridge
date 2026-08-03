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
#include <alpacacore/vendor/zwo/zwo_video_capture.h>

#include <limits>

namespace alpacacore::vendor::zwo {

ZWOVideoCapture::ZWOVideoCapture(int camera_id) : camera_id_(camera_id) {}

ZWOImageType ZWOVideoCapture::to_zwo_image_type(video::VideoPixelFormat format) {
    switch (format) {
        case video::VideoPixelFormat::Rgb24:
            return ZWOImageType::Rgb24;
        case video::VideoPixelFormat::Mono16:
        case video::VideoPixelFormat::BayerRggb16:
        case video::VideoPixelFormat::BayerGrbg16:
        case video::VideoPixelFormat::BayerGbrg16:
        case video::VideoPixelFormat::BayerBggr16:
            return ZWOImageType::Raw16;
        default:
            return ZWOImageType::Raw8;
    }
}

void ZWOVideoCapture::start(const video::VideoRequest& request) {
    if (started_) {
        throw AlpacaException("ZWOVideoCapture: already started", AlpacaError::InvalidOperation);
    }
    if (video::frame_bytes(request) == 0) {
        throw AlpacaException("ZWOVideoCapture: invalid frame geometry", AlpacaError::InvalidValue);
    }
    auto& sdk = ZWOSDKWrapper::instance();
    // ZWO ROI divisors (width % 8, height % 2 after binning) are the
    // caller's contract here — the Alpaca-facing alignment/padding logic
    // lives with the camera driver per the shared ROI-alignment rule; this
    // glue passes the SDK-ready geometry straight through.
    sdk.set_roi_format(camera_id_, request.width, request.height, request.bin, to_zwo_image_type(request.format));
    sdk.set_start_pos(camera_id_, request.start_x, request.start_y);
    sdk.set_control_value(camera_id_, ZWOControlType::Gain, request.gain, false);
    sdk.set_control_value(camera_id_, ZWOControlType::Exposure, static_cast<long>(request.exposure_ms) * 1000, false);
    sdk.start_video_capture(camera_id_);
    started_ = true;
}

bool ZWOVideoCapture::get_frame(std::uint8_t* buffer, std::size_t size, int timeout_ms) {
    if (!started_) {
        throw AlpacaException("ZWOVideoCapture: not started", AlpacaError::InvalidOperation);
    }
    if (size > static_cast<std::size_t>(std::numeric_limits<long>::max())) {
        throw AlpacaException("ZWOVideoCapture: buffer too large", AlpacaError::InvalidValue);
    }
    return ZWOSDKWrapper::instance().get_video_data(camera_id_, buffer, static_cast<long>(size), timeout_ms);
}

void ZWOVideoCapture::stop() {
    if (!started_) {
        return;
    }
    ZWOSDKWrapper::instance().stop_video_capture(camera_id_);
    started_ = false;
}

std::uint64_t ZWOVideoCapture::dropped_frames() {
    if (!started_) {
        return 0;
    }
    const int dropped = ZWOSDKWrapper::instance().get_dropped_frames(camera_id_);
    return dropped > 0 ? static_cast<std::uint64_t>(dropped) : 0;
}

}  // namespace alpacacore::vendor::zwo

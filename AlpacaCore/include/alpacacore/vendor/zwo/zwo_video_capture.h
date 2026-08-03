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

#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>
#include <alpacacore/video/video_capture.h>

namespace alpacacore::vendor::zwo {

// §77.1 ZWO glue behind the vendor-neutral VideoCapture seam
// (ASIStartVideoCapture / ASIGetVideoData / ASIStopVideoCapture).
//
// The camera identified by camera_id must already be open + initialized
// (the §77.2 mode-arbitration gate owns that hand-off; until it lands the
// caller opens the camera through ZWOSDKWrapper directly). This class only
// configures geometry/gain/exposure and runs the video streaming session.
class ZWOVideoCapture final : public video::VideoCapture {
public:
    explicit ZWOVideoCapture(int camera_id);

    void start(const video::VideoRequest& request) override;
    bool get_frame(std::uint8_t* buffer, std::size_t size, int timeout_ms) override;
    void stop() override;
    std::uint64_t dropped_frames() override;

private:
    static ZWOImageType to_zwo_image_type(video::VideoPixelFormat format);

    int camera_id_;
    bool started_{false};
};

}  // namespace alpacacore::vendor::zwo

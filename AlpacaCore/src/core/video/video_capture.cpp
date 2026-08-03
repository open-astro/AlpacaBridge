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

#include <alpacacore/video/video_capture.h>

namespace alpacacore::video {

int bits_per_plane(VideoPixelFormat format) {
    switch (format) {
        case VideoPixelFormat::Mono16:
        case VideoPixelFormat::BayerRggb16:
        case VideoPixelFormat::BayerGrbg16:
        case VideoPixelFormat::BayerGbrg16:
        case VideoPixelFormat::BayerBggr16:
            return 16;
        default:
            return 8;
    }
}

int plane_count(VideoPixelFormat format) { return format == VideoPixelFormat::Rgb24 ? 3 : 1; }

std::int32_t ser_color_id(VideoPixelFormat format) {
    switch (format) {
        case VideoPixelFormat::BayerRggb8:
        case VideoPixelFormat::BayerRggb16:
            return 8;
        case VideoPixelFormat::BayerGrbg8:
        case VideoPixelFormat::BayerGrbg16:
            return 9;
        case VideoPixelFormat::BayerGbrg8:
        case VideoPixelFormat::BayerGbrg16:
            return 10;
        case VideoPixelFormat::BayerBggr8:
        case VideoPixelFormat::BayerBggr16:
            return 11;
        case VideoPixelFormat::Rgb24:
            return 100;
        default:
            return 0;
    }
}

std::size_t frame_bytes(const VideoRequest& request) {
    if (request.width <= 0 || request.height <= 0) {
        return 0;
    }
    const std::size_t pixels = static_cast<std::size_t>(request.width) * static_cast<std::size_t>(request.height);
    return pixels * static_cast<std::size_t>(plane_count(request.format)) *
           static_cast<std::size_t>(bits_per_plane(request.format) / 8);
}

}  // namespace alpacacore::video

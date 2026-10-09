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

#include <alpacacore/util/image_validation.h>
#include <alpacacore/vendor/svbony/svbony_sdk_wrapper.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace alpacacore::vendor::svbony {

inline std::size_t required_frame_storage(int width, int height, SVBImageType type) {
    if (width <= 0 || height <= 0) {
        util::throw_invalid_camera_image("SVBONY SDK frame has invalid dimensions");
    }
    std::size_t bytes_per_pixel = 0;
    switch (type) {
        case SVBImageType::Raw8:
        case SVBImageType::Y8:
            bytes_per_pixel = 1;
            break;
        case SVBImageType::Raw16:
        case SVBImageType::Y16:
            bytes_per_pixel = 2;
            break;
        case SVBImageType::Rgb24:
            bytes_per_pixel = 3;
            break;
        case SVBImageType::Rgb32:
        case SVBImageType::Unknown:
            util::throw_invalid_camera_image("SVBONY SDK returned an unsupported image format");
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    if (w > kMax / h || w * h > kMax / bytes_per_pixel) {
        util::throw_invalid_camera_image("SVBONY SDK frame dimensions overflow buffer size");
    }
    return w * h * bytes_per_pixel;
}

inline void validate_frame_storage(std::span<const std::uint8_t> data, int width, int height, SVBImageType type) {
    if (data.size() < required_frame_storage(width, height, type)) {
        util::throw_invalid_camera_image("SVBONY frame storage is shorter than its ROI and format require");
    }
}

}  // namespace alpacacore::vendor::svbony

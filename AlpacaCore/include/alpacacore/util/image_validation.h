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
#include <alpacacore/util/error_handling.h>

#include <cstddef>
#include <limits>
#include <string>

namespace alpacacore::util {

struct ImageShape {
    std::size_t width;
    std::size_t height;
    std::size_t channels;
    std::size_t element_count;
};

[[noreturn]] inline void throw_invalid_camera_image(const char* reason) {
    throw AlpacaException(std::string("Camera returned invalid image data: ") + reason, AlpacaError::DriverException);
}

inline ImageShape validate_image_shape(const ImageArray& image) {
    if ((image.rank != 2 && image.rank != 3) || image.width <= 0 || image.height <= 0) {
        throw_invalid_camera_image("invalid rank or dimensions");
    }

    const auto width = static_cast<std::size_t>(image.width);
    const auto height = static_cast<std::size_t>(image.height);
    const std::size_t channels = image.rank == 3 ? 3 : 1;
    constexpr auto kMaxSize = std::numeric_limits<std::size_t>::max();
    if (width > kMaxSize / height) {
        throw_invalid_camera_image("dimensions overflow the payload size");
    }
    const std::size_t pixels = width * height;
    if (pixels > kMaxSize / channels) {
        throw_invalid_camera_image("dimensions overflow the payload size");
    }
    return {width, height, channels, pixels * channels};
}

inline void validate_image_data_length(const ImageArray& image, const ImageShape& shape) {
    if (shape.element_count != image.data.size()) {
        throw_invalid_camera_image("data length does not match dimensions");
    }
}

inline ImageShape validate_image_array(const ImageArray& image) {
    const ImageShape shape = validate_image_shape(image);
    validate_image_data_length(image, shape);
    return shape;
}

}  // namespace alpacacore::util

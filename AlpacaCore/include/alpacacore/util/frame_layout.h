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
#include <alpacacore/util/image_validation.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace alpacacore::util {

// A camera driver describes the frame the SDK handed it and the window the client asked for;
// build_frame_image returns the ImageArray. Every camera driver builds its image here, so the
// byte order, the stride, the crop and the size checks are written once.

enum class FrameFormat : std::uint8_t {
    Mono8,   // one byte per pixel
    Mono16,  // two bytes per pixel, little-endian (low byte first)
    Bgr24,   // three bytes per pixel, B,G,R: the output planes are R,G,B
    Rgb24,   // three bytes per pixel, R,G,B: copied as is
};

// What a window pixel with no source pixel behind it receives.
enum class FrameEdgeRule : std::uint8_t {
    ZeroFill,      // 0
    ReplicateOne,  // the nearest source pixel, for at most one missing row and one missing column; more is refused
};

// The SDK buffer: rows of stride_px pixels, src_width x src_height of them are image.
struct FrameLayout {
    FrameFormat format{FrameFormat::Mono8};
    std::size_t stride_px{0};
    int src_width{0};
    int src_height{0};
};

// The window the client gets: out_width x out_height pixels starting crop_x, crop_y into the source.
struct FrameWindow {
    int crop_x{0};
    int crop_y{0};
    int out_width{0};
    int out_height{0};
};

inline std::size_t frame_format_bytes_per_pixel(FrameFormat format) {
    switch (format) {
        case FrameFormat::Mono8:
            return 1;
        case FrameFormat::Mono16:
            return 2;
        case FrameFormat::Bgr24:
        case FrameFormat::Rgb24:
            return 3;
    }
    return 0;
}

// `vendor` prefixes every refusal ("ZWO frame buffer is shorter than ...").
inline ImageArray build_frame_image(const std::vector<std::uint8_t>& buffer, const FrameLayout& layout,
                                    const FrameWindow& window, FrameEdgeRule edge, std::string_view vendor) {
    const auto refuse = [&vendor](const char* what) {
        throw_invalid_camera_image((std::string(vendor) + " " + what).c_str());
    };
    if (window.out_width <= 0 || window.out_height <= 0 || layout.src_width <= 0 || layout.src_height <= 0 ||
        window.crop_x < 0 || window.crop_y < 0 || layout.stride_px < static_cast<std::size_t>(layout.src_width)) {
        refuse("frame has invalid dimensions");
    }
    const std::size_t bpp = frame_format_bytes_per_pixel(layout.format);
    const std::size_t channels = bpp == 3 ? 3 : 1;
    const std::size_t sample_bytes = bpp / channels;
    const auto src_w = static_cast<std::size_t>(layout.src_width);
    const auto src_h = static_cast<std::size_t>(layout.src_height);
    constexpr auto kMaxSize = std::numeric_limits<std::size_t>::max();
    if (layout.stride_px > kMaxSize / src_h || layout.stride_px * src_h > kMaxSize / bpp) {
        refuse("frame dimensions overflow the buffer size");
    }
    if (layout.stride_px * src_h * bpp > buffer.size()) {
        refuse("frame buffer is shorter than the reported ROI");
    }

    // How far the window runs past the source on each axis.
    const auto crop_x = static_cast<std::size_t>(window.crop_x);
    const auto crop_y = static_cast<std::size_t>(window.crop_y);
    const auto out_w = static_cast<std::size_t>(window.out_width);
    const auto out_h = static_cast<std::size_t>(window.out_height);
    const bool replicate = edge == FrameEdgeRule::ReplicateOne;
    if (replicate && (crop_x + out_w > src_w + 1 || crop_y + out_h > src_h + 1)) {
        refuse("frame is more than one pixel smaller than the requested ROI");
    }

    ImageArray image;
    image.width = window.out_width;
    image.height = window.out_height;
    image.rank = channels == 3 ? 3 : 2;
    image.data.assign(validate_image_shape(image).element_count, 0);

    for (std::size_t row = 0; row < out_h; ++row) {
        std::size_t src_row = row + crop_y;
        if (src_row >= src_h) {
            if (!replicate) break;
            src_row = src_h - 1;
        }
        for (std::size_t col = 0; col < out_w; ++col) {
            std::size_t src_col = col + crop_x;
            if (src_col >= src_w) {
                if (!replicate) break;
                src_col = src_w - 1;
            }
            const std::uint8_t* px = buffer.data() + (src_row * layout.stride_px + src_col) * bpp;
            std::int32_t* out = image.data.data() + (row * out_w + col) * channels;
            if (layout.format == FrameFormat::Bgr24) {
                out[0] = px[2];
                out[1] = px[1];
                out[2] = px[0];
            } else if (layout.format == FrameFormat::Rgb24) {
                out[0] = px[0];
                out[1] = px[1];
                out[2] = px[2];
            } else if (sample_bytes == 2) {
                out[0] = static_cast<std::int32_t>(static_cast<std::uint16_t>(px[0] | (px[1] << 8)));
            } else {
                out[0] = px[0];
            }
        }
    }
    validate_image_array(image);
    return image;
}

}  // namespace alpacacore::util

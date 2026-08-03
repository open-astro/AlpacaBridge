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

#include <cstddef>
#include <cstdint>

namespace alpacacore::video {

// Pixel layouts supported by the SER container. SER encodes layout as a
// ColorID plus a separate per-plane bit depth; ser_color_id() and
// bits_per_plane() derive both from one of these tokens.
enum class VideoPixelFormat {
    Mono8,
    Mono16,
    BayerRggb8,
    BayerGrbg8,
    BayerGbrg8,
    BayerBggr8,
    BayerRggb16,
    BayerGrbg16,
    BayerGbrg16,
    BayerBggr16,
    Rgb24
};

// Bits per pixel plane for a format (8 or 16); RGB24 is 8 per plane.
int bits_per_plane(VideoPixelFormat format);

// Number of image planes (1 for mono/Bayer, 3 for RGB).
int plane_count(VideoPixelFormat format);

// SER ColorID token for a format (MONO=0, BAYER_RGGB..BGGR=8..11, RGB=100).
std::int32_t ser_color_id(VideoPixelFormat format);

struct VideoRequest {
    int start_x{};
    int start_y{};
    int width{};
    int height{};
    int bin{1};
    VideoPixelFormat format{VideoPixelFormat::Mono8};
    long gain{};
    int exposure_ms{10};
};

// Bytes of one frame for a request (width * height * planes * bytes/plane).
std::size_t frame_bytes(const VideoRequest& request);

// Vendor seam for high-speed video capture (playbook §77.1). One
// implementation per vendor SDK; the ring buffer / SER writer / preview tap
// sit on top of this interface and stay vendor-agnostic.
//
// Pull model: the recorder's capture thread calls get_frame in a tight loop.
// Pull matches the ZWO / SVBONY / Player One SDKs directly; callback-push
// SDKs (ToupTek) adapt with an internal single-frame handoff in their glue.
class VideoCapture {
public:
    virtual ~VideoCapture() = default;

    // Configure the device and enter video mode. Throws AlpacaException on
    // vendor failure. Calling start while started is an error.
    virtual void start(const VideoRequest& request) = 0;

    // Block up to timeout_ms for the next frame and copy it into buffer
    // (size must be >= frame_bytes of the started request). Returns true
    // when a frame was delivered, false on timeout. Throws AlpacaException
    // on vendor failure.
    virtual bool get_frame(std::uint8_t* buffer, std::size_t size, int timeout_ms) = 0;

    // Leave video mode. Safe to call when already stopped.
    virtual void stop() = 0;

    // Frames the vendor SDK itself dropped since start (device/USB side —
    // distinct from ring-buffer drops, which the recorder counts). Vendors
    // with no such counter return 0.
    virtual std::uint64_t dropped_frames() = 0;
};

}  // namespace alpacacore::video

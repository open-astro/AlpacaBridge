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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/gphoto/gphoto_camera_driver.h>

#include <atomic>
#include <functional>

namespace alpacacore::test {

/**
 * Fake RawDecoder (issue #489 fault-injection seam): returns a canned
 * DecodedFrame instead of running libraw over real RAW bytes -- no RAW
 * fixtures are committed to this repo, so the libraw decode path itself
 * stays untested by design; this fake only exercises the driver code that
 * consumes a DecodedFrame (apply_decoded_frame_locked, geometry caching,
 * ROI cropping).
 */
class FakeRawDecoder : public vendor::gphoto::RawDecoder {
public:
    // 8x6 RGGB ramp by default: distinct from an all-zero frame so cropping
    // and pixel-copy bugs are visible in assertions.
    vendor::gphoto::DecodedFrame canned_frame = [] {
        vendor::gphoto::DecodedFrame frame;
        frame.width = 8;
        frame.height = 6;
        frame.bayer_offset_x = 0;
        frame.bayer_offset_y = 0;
        frame.max_adu = 65535;
        frame.sensor_type = alpacacore::SensorType::RGGB;
        frame.pixels.resize(static_cast<std::size_t>(frame.width) * frame.height);
        for (int row = 0; row < frame.height; ++row) {
            for (int col = 0; col < frame.width; ++col) {
                frame.pixels[static_cast<std::size_t>(row) * frame.width + col] = row * frame.width + col;
            }
        }
        return frame;
    }();

    bool should_throw{false};
    // Runs inside decode() on the calling thread before the frame returns; a test blocks here to hold a decode
    // open (ADR 0015 rule 5: an explicit hook, never a sleep).
    std::function<void()> on_decode;
    // Atomic because the stress harness hands one decoder to a driver that
    // decodes from the connect-priming and exposure threads. The driver
    // serializes those today (connecting_priming_ under mutex_), but this is
    // the same trap LockedGPhotoSDK exists to avoid: a fake counter must not
    // be what lights up ThreadSanitizer.
    std::atomic<int> decode_call_count{0};

    vendor::gphoto::DecodedFrame decode(const std::vector<std::uint8_t>&) override {
        ++decode_call_count;
        if (on_decode) on_decode();
        if (should_throw) {
            throw AlpacaException("FakeRawDecoder: injected decode failure", AlpacaError::DriverException);
        }
        return canned_frame;
    }
};

}  // namespace alpacacore::test

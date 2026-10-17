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
#include <alpacacore/util/task_clock.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace alpacacore::vendor::gphoto {

class GPhotoSDK;  // fault-injection seam (gphoto_sdk_wrapper.h, issue #489)

/**
 * @brief Result of decoding a captured RAW frame with libraw: pixel data plus
 * the geometry/metadata the driver needs (sensor size, Bayer phase, max ADU,
 * optional sensor temperature).
 */
struct DecodedFrame {
    std::vector<std::int32_t> pixels;
    int width{};
    int height{};
    int bayer_offset_x{};
    int bayer_offset_y{};
    int max_adu{65535};
    SensorType sensor_type{SensorType::RGGB};
    std::optional<double> sensor_temperature;
};

/**
 * @brief Abstract interface over decoding a captured RAW file's bytes into a
 * DecodedFrame.
 *
 * This is a second fault-injection seam (issue #489), separate from
 * GPhotoSDK: libraw is a second hardware-shaped dependency (no RAW fixtures
 * are committed to the repo -- see LibRawDecoder below), so tests substitute
 * a fake returning a canned DecodedFrame (AlpacaCore/tests/fake_raw_decoder.h)
 * instead of decoding real bytes.
 */
class RawDecoder {
public:
    // Protected, non-virtual destructor for the same reason as GPhotoSDK:
    // nothing owns a RawDecoder*. The driver holds a RawDecoder&, and every
    // implementation is a function-local static or a stack object.
    virtual DecodedFrame decode(const std::vector<std::uint8_t>& raw_bytes) = 0;

protected:
    ~RawDecoder() = default;
};

/// Real libraw-backed decoder used in production.
class LibRawDecoder final : public RawDecoder {
public:
    DecodedFrame decode(const std::vector<std::uint8_t>& raw_bytes) override;
};

/**
 * @brief Create a libgphoto2-backed DSLR/mirrorless camera driver.
 *
 * @param device_number Alpaca device number
 * @param camera_index libgphoto2 USB autodetect index (0-based), matching
 *        the SDK-enumerated convention used by the other camera vendors.
 * @return Unique pointer to camera driver
 */
std::unique_ptr<CameraDriver> create_gphoto_camera(int device_number, int camera_index);

// Test seam: identical driver wired to an injected SDK and RAW decoder. Every
// driver wait and deadline reads `clock` (decision 0005); tests pass a
// FakeTaskClock. `clock` must outlive the driver.
std::unique_ptr<CameraDriver> create_gphoto_camera(int device_number, int camera_index, GPhotoSDK& sdk,
                                                   RawDecoder& decoder,
                                                   util::TaskClock& clock = util::default_task_clock());

}  // namespace alpacacore::vendor::gphoto

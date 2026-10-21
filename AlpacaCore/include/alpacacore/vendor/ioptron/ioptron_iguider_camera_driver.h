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

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace alpacacore::vendor::ioptron {

class IGuiderV4L2;

/**
 * The iGuider's exposure steps. The firmware reads V4L2_CID_EXPOSURE_ABSOLUTE as
 * a step code, not as 100 us units: tick 2500 is a 3.5 s exposure. The pairs
 * were sniffed from the vendor's Windows driver on the USB bus (INDI PR #2012)
 * and match its ASCOM driver's 12-step rounding (1.3.2.0).
 */
inline constexpr std::array<double, 12> kIGuiderDurations{0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
                                                          1.0,  1.5,  2.0,  2.5, 3.0, 3.5};
inline constexpr std::array<std::int32_t, 12> kIGuiderTicks{1, 2, 5, 10, 20, 39, 78, 156, 312, 625, 1250, 2500};

/// Index of the step nearest @p seconds (the lower one on a tie), as INDI and the vendor driver round.
inline std::size_t iguider_exposure_step(double seconds) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < kIGuiderDurations.size(); ++i) {
        if (std::fabs(kIGuiderDurations[i] - seconds) < std::fabs(kIGuiderDurations[best] - seconds)) best = i;
    }
    return best;
}

/// Index of the step whose tick code is @p ticks (0 when it is none of them).
inline std::size_t iguider_tick_step(std::int32_t ticks) {
    for (std::size_t i = 0; i < kIGuiderTicks.size(); ++i) {
        if (kIGuiderTicks[i] == ticks) return i;
    }
    return 0;
}

/**
 * @brief Create an iOptron iGuider guide camera driver.
 *
 * @param device_number Alpaca device number
 * @param device_path   V4L2 node to open (/dev/videoN or a /dev/v4l/by-id link);
 *                      empty finds the first iGuider at connect.
 */
std::unique_ptr<CameraDriver> create_iguider_camera(int device_number, const std::string& device_path = "");

/**
 * @brief Test seam: as create_iguider_camera(), over an injected V4L2 seam.
 *
 * @p v4l2 and @p clock must outlive the driver. Every driver deadline reads
 * @p clock (decision 0005).
 */
std::unique_ptr<CameraDriver> create_iguider_camera(int device_number, const std::string& device_path,
                                                    IGuiderV4L2& v4l2,
                                                    util::TaskClock& clock = util::default_task_clock());

}  // namespace alpacacore::vendor::ioptron

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
#include <alpacacore/vendor/zwo/zwo_camera_identity.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

/**
 * @brief Create a ZWO camera driver by camera ID.
 *
 * @param device_number Alpaca device number
 * @param camera_id ZWO SDK camera ID
 * @return Unique pointer to camera driver
 */
std::unique_ptr<CameraDriver> create_zwo_camera(int device_number, int camera_id);

/**
 * @brief Create a ZWO camera driver by camera index (enumeration order).
 *
 * @param device_number Alpaca device number
 * @param camera_index ZWO SDK camera index (0-based)
 * @return Unique pointer to camera driver
 */
std::unique_ptr<CameraDriver> create_zwo_camera_by_index(int device_number, int camera_index);

/**
 * @brief What a ZWO camera config entry binds a driver to.
 *
 * \p identity is resolved against a fresh enumeration on every connect
 * (serial first, then model name, then the id/index hints), so a camera
 * re-plugged under another index still binds by serial. \p unique_id is the
 * stored UniqueID a serial-less body reports; \p claimed_serials are the
 * serials other config entries bind, which a serial-less entry skips.
 */
struct ZwoCameraBinding {
    ZwoConfiguredIdentity identity;
    std::string unique_id;
    std::set<std::string> claimed_serials;
};

/// Every connected camera with its serial (opens each one briefly).
std::vector<ZwoEnumeratedCamera> enumerate_zwo_cameras(const std::string& only_model_name = {});

/// Create a camera driver for a config entry. See ZwoCameraBinding.
std::unique_ptr<CameraDriver> create_zwo_camera_bound(int device_number, const ZwoCameraBinding& binding);

} // namespace alpacacore::vendor::zwo

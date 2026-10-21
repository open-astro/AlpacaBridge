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

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/task_clock.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace alpacacore::vendor::celestron {

struct CelestronCgxConnectionInfo {
    std::string port_path;
    std::optional<double> site_latitude_degrees;
    std::optional<double> site_longitude_degrees;
    std::optional<double> site_elevation_meters;
    double aperture_diameter_meters = 0.0;
    double focal_length_meters = 0.0;
};

using CgxSystemNow = std::function<std::chrono::system_clock::time_point()>;

/// `system_now` is a deterministic wall-clock seam for UTCDate/pointing tests.
std::unique_ptr<TelescopeDriver> create_celestron_cgx_telescope(int device_number,
                                                                const CelestronCgxConnectionInfo& info,
                                                                util::TaskClock& clock = util::default_task_clock(),
                                                                CgxSystemNow system_now = {});

}  // namespace alpacacore::vendor::celestron

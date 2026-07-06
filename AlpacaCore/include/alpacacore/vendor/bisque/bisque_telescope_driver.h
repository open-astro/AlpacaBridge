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
#include <alpacacore/vendor/bisque/bisque_protocol_wrapper.h>
#include <memory>
#include <optional>

namespace alpacacore::vendor::bisque {

std::unique_ptr<TelescopeDriver> create_bisque_telescope(
    int device_number,
    const ConnectionInfo& connection_info);

std::unique_ptr<TelescopeDriver> create_bisque_telescope_with_site(
    int device_number,
    const ConnectionInfo& connection_info,
    std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m);

} // namespace alpacacore::vendor::bisque

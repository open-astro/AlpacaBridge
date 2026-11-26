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

// The configuration every remotealpaca proxied device shares (decision 0023, issue #1753): which upstream device it
// serves and how often it polls it.

#include <chrono>
#include <string>

namespace alpacacore::vendor::remotealpaca {

// The poll period: 1 to 60 seconds, 2 by default (#1753 maintainer decision, 2026-11-16).
inline constexpr std::chrono::seconds kDefaultPollPeriod{2};
inline constexpr std::chrono::seconds kMinPollPeriod{1};
inline constexpr std::chrono::seconds kMaxPollPeriod{60};

struct RemoteDeviceConfig {
    std::string host;                // the upstream server: a name or an IP address, no scheme, no brackets
    int port = 0;                    // 1 to 65535
    int upstream_device_number = 0;  // the upstream's own device number of the proxied type
    std::chrono::seconds poll_period{kDefaultPollPeriod};
};

}  // namespace alpacacore::vendor::remotealpaca

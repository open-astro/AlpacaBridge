// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

// ALPACAHTTP_VERSION is defined by CMake from the VERSION file
// If this header is included without going through CMake, this will fail to compile
// which is intentional - the version should always come from the VERSION file
#ifndef ALPACAHTTP_VERSION
#error "ALPACAHTTP_VERSION must be defined by CMake. Ensure you're building through CMake."
#endif

namespace alpacahttp {

inline constexpr const char* kVersion = ALPACAHTTP_VERSION;

} // namespace alpacahttp

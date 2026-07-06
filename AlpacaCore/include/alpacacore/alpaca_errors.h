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

namespace alpacacore {

/**
 * @brief Canonical ASCOM Alpaca error numbers.
 *
 * This namespace is the single source of truth for Alpaca error codes;
 * AlpacaHTTP's util::ErrorCode aliases these values so the two layers cannot
 * drift. 0x400-0x4FF are the ASCOM-reserved range; 0x500-0xFFF is the
 * driver-specific range (DriverException is the generic catch-all there).
 */
namespace AlpacaError {
    constexpr int Success = 0;
    constexpr int NotImplemented = 0x400;         // 1024
    constexpr int PropertyNotImplemented = 0x400; // 1024
    constexpr int MethodNotImplemented = 0x400;   // 1024
    constexpr int InvalidValue = 0x401;           // 1025
    constexpr int ValueNotSet = 0x402;            // 1026
    constexpr int NotConnected = 0x407;           // 1031
    constexpr int InvalidWhileParked = 0x408;     // 1032
    constexpr int InvalidWhileSlaved = 0x409;     // 1033
    constexpr int InvalidOperation = 0x40B;       // 1035
    constexpr int ActionNotImplemented = 0x40C;   // 1036
    constexpr int NotInCacheException = 0x40D;    // 1037
    constexpr int UnspecifiedError = 0x4FF;       // 1279
    constexpr int DriverException = 0x500;        // 1280 (generic driver error)
}

} // namespace alpacacore

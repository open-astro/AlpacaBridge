// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#pragma once

namespace alpacacore {

/**
 * @brief Alpaca error numbers.
 *
 * These correspond to ASCOM Alpaca error codes.
 * Higher layers map AlpacaException to these error numbers.
 */
namespace AlpacaError {
    constexpr int Success = 0;
    constexpr int InvalidValue = 0x400;           // 1024
    constexpr int ValueNotSet = 0x401;            // 1025
    constexpr int NotConnected = 0x407;           // 1031
    constexpr int NotImplemented = 0x43F;         // 1087
    constexpr int InvalidOperationException = 0x440; // 1088
    constexpr int ActionNotImplemented = 0x441;   // 1089
    constexpr int DriverException = 0x500;        // 1280
    constexpr int InvalidOperation = 0x501;        // 1281
    constexpr int NotSupported = 0x503;           // 1283
    constexpr int PropertyNotImplemented = 0x504;  // 1284
    constexpr int MethodNotImplemented = 0x505;   // 1285
    constexpr int Slaved = 0x506;                 // 1286
    constexpr int Parked = 0x507;                 // 1287
    constexpr int InvalidWhileParked = 0x508;     // 1288
    constexpr int InvalidWhileSlaved = 0x509;     // 1289
    constexpr int NotAtHome = 0x50A;              // 1290
    constexpr int InvalidWhileSlewing = 0x50B;    // 1291
    constexpr int InvalidOperationException2 = 0x50C; // 1292
    constexpr int UnspecifiedError = 0x500;       // 1280
}

} // namespace alpacacore


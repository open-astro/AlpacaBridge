// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#pragma once

// ALPACACORE_VERSION is defined by CMake from the VERSION file
// If this header is included without going through CMake, this will fail to compile
// which is intentional - the version should always come from the VERSION file
#ifndef ALPACACORE_VERSION
#error "ALPACACORE_VERSION must be defined by CMake. Ensure you're building through CMake."
#endif

namespace alpacacore {

inline constexpr const char* kVersion = ALPACACORE_VERSION;

}  // namespace alpacacore

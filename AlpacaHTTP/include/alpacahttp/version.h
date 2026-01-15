// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

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

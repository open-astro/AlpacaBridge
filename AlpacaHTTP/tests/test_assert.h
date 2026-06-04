// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
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

#include <cstdio>
#include <cstdlib>

// EXPECT: an always-on test check for the hand-rolled (non-Catch2) AlpacaHTTP
// tests. Unlike <cassert>'s assert(), it is NOT compiled out under -DNDEBUG, so
// the test verifies the same thing regardless of CMAKE_BUILD_TYPE. This matters
// because several checks wrap side-effecting calls (e.g. request.parse(...)):
// with assert() under a Release/NDEBUG build those calls vanish entirely, the
// test silently exercises nothing, and later code crashes on the empty state.
//
// The expression is evaluated exactly once (in the if condition) so side
// effects fire as intended. On failure it prints the expression + location and
// aborts, which ctest reports as a failed test.
#define EXPECT(expr)                                                                            \
    do {                                                                                        \
        if (!(expr)) {                                                                          \
            std::fprintf(stderr, "EXPECT failed: %s\n  at %s:%d\n", #expr, __FILE__, __LINE__); \
            std::abort();                                                                       \
        }                                                                                       \
    } while (0)

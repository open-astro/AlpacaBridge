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
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#pragma once

#include <mutex>
#include <shared_mutex>

namespace alpacacore::threading {

/**
 * @brief Thread-safe wrapper utilities.
 *
 * Device instances manage their own internal locking.
 * Higher layers manage concurrency between device instances.
 */

// Standard mutex types are sufficient for now
// This header provides a namespace for future threading utilities

} // namespace alpacacore::threading


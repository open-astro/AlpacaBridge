// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://www.mongodb.com/licensing/server-side-public-license
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

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


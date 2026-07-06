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


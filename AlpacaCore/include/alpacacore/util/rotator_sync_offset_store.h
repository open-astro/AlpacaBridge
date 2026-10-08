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

#include <string>

namespace alpacacore::util {

// Persists the IRotatorV4 Sync offset (degrees, Position - MechanicalPosition)
// per device, so it survives a reconnect, a service restart and a device reboot.
//
// One flat JSON object in a state file, key = driver unique id, value = offset.
// AlpacaCore carries no JSON library, so this reads and writes only that shape.
// Every call is mutex-guarded and never throws: a missing, unreadable or corrupt
// file loads as 0, and a failed save logs a WARNING and returns false, so Sync
// and Connect never fail on storage (docs/decisions/0008).
class RotatorSyncOffsetStore {
public:
    // Default file, relative to the working directory: config/rotator_sync_offsets.json.
    static std::string default_path();

    // Test seam: replace the process-wide default path; an empty string restores
    // the built-in default. Returns the previous override (empty if none).
    static std::string set_default_path(const std::string& path);

    // Offset for @p key, or 0 when there is no usable entry.
    static double load(const std::string& key);

    // Writes @p offset_degrees for @p key (temp file + rename), keeping other keys.
    static bool save(const std::string& key, double offset_degrees);
};

}  // namespace alpacacore::util

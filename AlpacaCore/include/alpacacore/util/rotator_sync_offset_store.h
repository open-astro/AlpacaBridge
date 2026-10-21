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

#include <cstddef>
#include <functional>
#include <set>
#include <string>

namespace alpacacore::util {

// Persists the IRotatorV4 Sync offset (degrees, Position - MechanicalPosition)
// per device, so it survives a reconnect, a service restart and a device reboot.
//
// One flat JSON object in a state file, key = driver unique id, value = offset.
// AlpacaCore carries no JSON library, so this reads and writes only that shape.
// Every call is mutex-guarded and never throws: a missing, unreadable or corrupt
// file loads as 0, so Connect never fails on storage, and a failed save logs a
// WARNING and returns false, so Sync can fail instead of losing the offset
// (docs/decisions/0008).
class RotatorSyncOffsetStore {
public:
    // Default file, relative to the working directory: config/rotator_sync_offsets.json.
    static std::string default_path();

    // Test seam: replace the process-wide default path; an empty string restores
    // the built-in default. Returns the previous override (empty if none).
    static std::string set_default_path(const std::string& path);

    // Test seam: called after every save() returns its result, with no store
    // lock held, so a test can hold one Sync between its save and its in-memory
    // write. Empty by default; tests clear it afterwards.
    static void set_after_save_hook_for_testing(std::function<void()> hook);

    // Offset for @p key, or 0 when there is no usable entry.
    static double load(const std::string& key);

    // Writes @p offset_degrees for @p key (temp file + rename), keeping other keys.
    // Returns false when the offset could not be stored; Sync then fails
    // (decision 0008), so the caller keeps its previous offset.
    static bool save(const std::string& key, double offset_degrees);

    // Offset for @p key. When @p key has no entry and @p legacy_key has one,
    // moves that entry to @p key and returns it: the one-time move from a
    // driver-built ID to the persisted GUID (decision 0018). A failed move
    // still returns the offset and leaves the legacy entry in place.
    static double load_or_adopt(const std::string& key, const std::string& legacy_key);

    // Removes every GUID-shaped key (util::is_unique_id) that is not in
    // @p live_unique_ids: the offsets of removed rotators. Driver-built legacy
    // keys are left alone. Returns how many entries went.
    static std::size_t prune_unknown_unique_ids(const std::set<std::string>& live_unique_ids);
};

}  // namespace alpacacore::util

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

#include <functional>
#include <optional>
#include <string>

namespace alpacacore::util {

// Remembers where an auto-detected device was last found, so the next connect
// tries that one place before it runs the full hardware search.
//
// One flat JSON object of strings in a state file, key = location_key() of the
// driver unique id (decision 0018) and its enumeration index, value = the
// location: a serial path, or "host:port" for a network device. It is runtime
// state, kept apart from the user's settings in registered_devices.json (the
// same split as decision 0008). AlpacaCore carries no JSON library, so this
// reads and writes only that shape. Every call is mutex-guarded and never
// throws: a missing, unreadable or corrupt file means no remembered location,
// and a failed save logs a WARNING and returns false, so a connect never fails
// on storage.
//
// Only the auto-detect path touches the store. A port or address the user
// configured is used as given and is never read from or written to it.
class DeviceLocationStore {
public:
    // Default file, relative to the working directory: config/device_locations.json.
    static std::string default_path();

    // Test seam: replace the process-wide default path; an empty string restores
    // the built-in default. Returns the previous override (empty if none).
    static std::string set_default_path(const std::string& path);

    // The remembered location for @p key, or nothing when there is no usable entry.
    static std::optional<std::string> load(const std::string& key);

    // Remembers @p location for @p key (temp file + rename), keeping other keys.
    // Writes nothing when the entry is already @p location. Returns false when
    // the location could not be stored; the caller carries on.
    static bool save(const std::string& key, const std::string& location);
};

// The store key for the unit @p unique_id as found at enumeration index
// @p index: "<id>#<index>". The index is part of the key because a user can
// renumber an entry (decision 0018 keeps its id), and a location found at the
// old index must not answer for the new one.
std::string location_key(const std::string& unique_id, int index);

// The location to remember for the serial port @p port_path: the
// /dev/serial/by-id symlink in @p by_id_dir that resolves to the same device
// (it survives a replug under another ttyUSBn), else @p port_path itself.
std::string serial_location_for(const std::string& port_path, const std::string& by_id_dir = "/dev/serial/by-id");

// "host:port" as stored for a network device, and its inverse. parse returns
// false for anything that is not a non-empty host, a colon and a port 1-65535
// (a serial path never parses).
std::string network_location_for(const std::string& host, int tcp_port);
bool parse_network_location(const std::string& location, std::string& host, int& tcp_port);

// The remembered serial location for @p key when it is still worth taking: it
// is a path, no connected driver holds it (decision 0017: a held port is never
// opened or probed) and @p handshake, the device's own identity probe, passes
// on it. Returns nothing otherwise, and the caller runs the full search.
std::optional<std::string> recall_serial_location(const std::string& key,
                                                  const std::function<bool(const std::string&)>& handshake);

}  // namespace alpacacore::util

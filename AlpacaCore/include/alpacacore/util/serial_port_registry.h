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

// Process-wide registry of serial-port paths currently held open by a
// connected protocol wrapper. Auto-detect scans consult it so probing one
// vendor's device never opens/reads a /dev node another connected device is
// using (two readers on the same node split the kernel receive buffer and
// starve the connected device's stream). Originally file-local to the
// WandererCover wrapper; promoted here once a second WandererAstro device
// type (the rotator) needed the same guarantee ACROSS wrappers.
//
// Paths should be canonical (std::filesystem::canonical) where resolvable so
// a by-id symlink and its /dev/ttyUSBn target compare equal.

#include <mutex>
#include <set>
#include <string>

namespace alpacacore::util {

namespace detail {
// Function-local statics avoid static-init-order issues.
inline std::mutex& serial_registry_mutex() {
    static std::mutex m;
    return m;
}
inline std::set<std::string>& serial_registry_set() {
    static std::set<std::string> s;
    return s;
}
}  // namespace detail

/** @brief Register @p path as held open by a connected device. */
inline void mark_serial_port_open(const std::string& path) {
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(detail::serial_registry_mutex());
    detail::serial_registry_set().insert(path);
}

/** @brief Remove @p path from the in-use registry. */
inline void mark_serial_port_closed(const std::string& path) {
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(detail::serial_registry_mutex());
    detail::serial_registry_set().erase(path);
}

/** @brief True if @p path is registered as held open. */
inline bool is_serial_port_in_use(const std::string& path) {
    std::lock_guard<std::mutex> lock(detail::serial_registry_mutex());
    return detail::serial_registry_set().count(path) > 0;
}

}  // namespace alpacacore::util

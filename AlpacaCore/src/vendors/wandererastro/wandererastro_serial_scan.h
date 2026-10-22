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

// Shared by the four WandererAstro auto-detect scans (cover, rotator, filter
// wheel, box): how one candidate port a connected device may hold is treated
// (issue #1202). Private to this vendor's sources.

#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_port_registry.h>

#include <string>
#include <vector>

namespace alpacacore::vendor::wandererastro::detail {

/**
 * @brief Decide whether a scan whose devices connect as @p owner may probe
 *        @p resolved.
 *
 * Returns true for a free port. A port another kind of connected device holds
 * is skipped without being opened. A port a connected device of this same
 * kind holds is not probed either, but gets a placeholder entry in
 * @p results at its scan position (port path and by-id name only), so "the
 * second WandererCover" stays the second one while the first is connected;
 * a driver that picks the placeholder is refused by its link claim, which
 * names the holder.
 */
template <typename PortInfo>
bool admit_serial_candidate(const std::string& resolved, const std::string& device_id, const char* owner,
                            std::vector<PortInfo>& results) {
    switch (util::serial_candidate_use(resolved, owner)) {
        case util::SerialCandidateUse::HeldByOther:
            ALPACA_LOG_DEBUG("WandererAstro",
                             std::string(owner) + " scan skipped " + resolved + ": held by another connected device");
            return false;
        case util::SerialCandidateUse::HeldBySibling: {
            ALPACA_LOG_DEBUG("WandererAstro", std::string(owner) + " scan kept " + resolved +
                                                  " in the auto-detect index without probing: a connected " + owner +
                                                  " holds it");
            PortInfo held;
            held.port_path = resolved;
            held.device_id = device_id;
            results.push_back(held);
            return false;
        }
        case util::SerialCandidateUse::Free:
            break;
    }
    return true;
}

}  // namespace alpacacore::vendor::wandererastro::detail

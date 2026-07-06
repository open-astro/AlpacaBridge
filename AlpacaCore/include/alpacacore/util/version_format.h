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

/**
 * @brief Normalize a comma/space-separated version to dotted form.
 *
 * Some vendor SDKs report a version like ZWO's `ASIGetSDKVersion()` →
 * `"1, 7, 7, 0"`. This renders it as the conventional `"1.7.7.0"`: spaces are
 * dropped and commas become dots; every other character passes through. Kept in
 * one place so a future SDK format change is a single edit (was previously
 * copy-pasted across the ZWO camera/EFW/EAF drivers).
 */
inline std::string normalize_dotted_version(const std::string& version) {
    std::string out;
    out.reserve(version.size());
    for (char c : version) {
        if (c == ' ') {
            continue;
        }
        out.push_back(c == ',' ? '.' : c);
    }
    return out;
}

}  // namespace alpacacore::util

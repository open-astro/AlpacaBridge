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

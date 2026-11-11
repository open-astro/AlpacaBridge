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

// A device's Name is its brand followed by its model (decision 0024). SDKs that
// report a model string sometimes include the brand ("ZWO ASI2600MC Pro",
// "QHY268M", "ALTAIR178M3") and sometimes not ("EAF", "AFW-M"); this adds the
// brand only when the string does not already start with it.

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace alpacacore::util {

/// True when @p name starts with @p brand, ignoring ASCII case.
inline bool starts_with_brand(std::string_view name, std::string_view brand) {
    return name.size() >= brand.size() && std::equal(brand.begin(), brand.end(), name.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

/// @p model as reported, prefixed with "@p brand " unless it already starts with the brand.
inline std::string branded_name(std::string_view brand, std::string_view model) {
    if (starts_with_brand(model, brand)) return std::string(model);
    return std::string(brand) + " " + std::string(model);
}

/// @p model without its " (...)" qualifiers, which some SDKs add for a connection mode (libgphoto2 names a Sony body
/// in PC-remote mode "Sony Alpha-A7 III (Control)"); the qualifier belongs in the Description.
inline std::string without_qualifiers(std::string_view model) {
    std::string out(model);
    for (auto open = out.find(" ("); open != std::string::npos; open = out.find(" (", open)) {
        const auto close = out.find(')', open);
        if (close == std::string::npos) break;
        out.erase(open, close + 1 - open);
    }
    return out;
}

}  // namespace alpacacore::util

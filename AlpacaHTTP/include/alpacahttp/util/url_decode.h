// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

#include <cstddef>
#include <string>

namespace alpacahttp::util {

namespace detail {
// Value of one hex digit, or -1 when c is not one.
inline int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
}  // namespace detail

// Decodes an application/x-www-form-urlencoded component (GET query and PUT
// form body): "%XX" with two hex digits becomes that byte, '+' becomes a space.
// A '%' not followed by two hex digits (bad digit or truncated) stays a literal
// '%' and the characters after it are kept as they are.
inline std::string url_decode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = detail::hex_digit_value(value[i + 1]);
            const int lo = detail::hex_digit_value(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                result.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        result.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return result;
}

}  // namespace alpacahttp::util

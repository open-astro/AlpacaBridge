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
#include <string_view>

namespace alpacahttp::util {

namespace detail {

// Length of the well-formed UTF-8 sequence starting at s[i], or 0 when the
// bytes there are not one (stray continuation, overlong, surrogate, > U+10FFFF,
// truncated).
inline std::size_t utf8_sequence_length(std::string_view s, std::size_t i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        return 1;
    }
    std::size_t len = 0;
    unsigned char lo = 0x80;
    unsigned char hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        len = 2;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        len = 3;
        if (b0 == 0xE0) {
            lo = 0xA0;
        } else if (b0 == 0xED) {
            hi = 0x9F;
        }
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        len = 4;
        if (b0 == 0xF0) {
            lo = 0x90;
        } else if (b0 == 0xF4) {
            hi = 0x8F;
        }
    } else {
        return 0;
    }
    if (i + len > s.size()) {
        return 0;
    }
    const auto b1 = static_cast<unsigned char>(s[i + 1]);
    if (b1 < lo || b1 > hi) {
        return 0;
    }
    for (std::size_t k = 2; k < len; ++k) {
        const auto bk = static_cast<unsigned char>(s[i + k]);
        if (bk < 0x80 || bk > 0xBF) {
            return 0;
        }
    }
    return len;
}

}  // namespace detail

// Makes a client-supplied string safe to put in a log line. Bytes below 0x20,
// 0x7f and bytes that are not well-formed UTF-8 become `\xNN`, so a client
// cannot forge or garble log lines. Output stops before the first code point
// that would end past `max_bytes` input bytes (never mid-sequence), and a cut
// appends `... (<original length> bytes)`.
inline std::string escape_for_log(std::string_view s, std::size_t max_bytes = 256) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t len = detail::utf8_sequence_length(s, i);
        const std::size_t step = len == 0 ? 1 : len;
        if (i + step > max_bytes) {
            out += "... (" + std::to_string(s.size()) + " bytes)";
            return out;
        }
        const auto b = static_cast<unsigned char>(s[i]);
        if (len == 0 || b < 0x20 || b == 0x7f) {
            out += "\\x";
            out += kHex[b >> 4];
            out += kHex[b & 0x0f];
        } else {
            out.append(s.data() + i, len);
        }
        i += step;
    }
    return out;
}

}  // namespace alpacahttp::util

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

// Pure NexStar framing helpers shared by the Celestron and SynScan wrappers,
// which speak the same hex-field protocol (#1741). SynScan's copy was the source.

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace alpacacore::util {

/// `value` as `width` upper-case hex digits, zero padded.
inline std::string build_hex(uint32_t value, std::size_t width) {
    std::ostringstream oss;
    oss << std::uppercase << std::setw(static_cast<int>(width)) << std::setfill('0') << std::hex << value;
    return oss.str();
}

/// The last run of hex digits in `value` ("12AB#" -> "12AB").
inline std::string trailing_hex(const std::string& value) {
    std::size_t end = value.size();
    while (end > 0 && !std::isxdigit(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    std::size_t start = end;
    while (start > 0 && std::isxdigit(static_cast<unsigned char>(value[start - 1]))) {
        --start;
    }
    return value.substr(start, end - start);
}

/// The first run of hex digits in `value` ("#12AB,34" -> "12AB").
inline std::string leading_hex(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && !std::isxdigit(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    std::size_t end = start;
    while (end < value.size() && std::isxdigit(static_cast<unsigned char>(value[end]))) {
        ++end;
    }
    return value.substr(start, end - start);
}

}  // namespace alpacacore::util

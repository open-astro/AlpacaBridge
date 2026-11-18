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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

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

/// The mode byte of a NexStar reply (tracking mode `t`, alignment `J`): a digit character reads as its value ('2'
/// is 2), any other byte as itself (a raw 2 is 2). An empty reply is 0. Moved here from both wrappers (#1757).
inline int parse_mode_byte(const std::string& response) {
    if (response.empty()) {
        return 0;
    }
    const auto ch = static_cast<unsigned char>(response[0]);
    if (ch >= static_cast<unsigned char>('0') && ch <= static_cast<unsigned char>('9')) {
        return static_cast<int>(ch - static_cast<unsigned char>('0'));
    }
    return static_cast<int>(ch);
}

/// The two hex fields of a NexStar axis-pair reply ("AAAA,BBBB#", or "AAAAAAAA,BBBBBBBB#" when @p precise), or
/// nullopt when a field has too few hex digits or does not parse. A precise field is eight digits, the position in
/// the upper six and two ignored ones (issue #785): of the first field the last eight digits are kept, then their
/// first six, so a stale hex byte left in front of the reply cannot shift it. Moved here from both wrappers
/// (#1757); the Celestron copy lacked the #785 trim.
inline std::optional<std::pair<uint32_t, uint32_t>> parse_axis_pair_response(const std::string& response,
                                                                             bool precise) {
    const std::size_t expected_digits = precise ? 6 : 4;
    const auto comma = response.find(',');
    if (comma == std::string::npos) {
        return std::nullopt;
    }

    std::string left = trailing_hex(response.substr(0, comma));
    std::string right = leading_hex(response.substr(comma + 1));
    if (left.size() < expected_digits || right.size() < expected_digits) {
        return std::nullopt;
    }

    const std::size_t field_digits = precise ? 8 : 4;
    if (left.size() > field_digits) {
        left = left.substr(left.size() - field_digits);
    }
    left.resize(expected_digits);
    right.resize(expected_digits);

    uint32_t first_raw = 0;
    uint32_t second_raw = 0;
    std::istringstream first_iss(left);
    std::istringstream second_iss(right);
    first_iss >> std::hex >> first_raw;
    second_iss >> std::hex >> second_raw;
    if (first_iss.fail() || second_iss.fail()) {
        return std::nullopt;
    }
    return std::make_pair(first_raw, second_raw);
}

/// The NexStar set-location command: 'W' and eight bytes, latitude degrees, minutes, seconds and sign (1 south),
/// then longitude degrees, minutes, seconds and sign (1 west). Each part is truncated, not rounded, as both
/// wrappers did (#1757).
inline std::string location_command(double latitude_degrees, double longitude_degrees) {
    const int lat_sign = latitude_degrees < 0.0 ? 1 : 0;
    const int lon_sign = longitude_degrees < 0.0 ? 1 : 0;
    const double lat = std::abs(latitude_degrees);
    const double lon = std::abs(longitude_degrees);

    const int lat_deg = static_cast<int>(lat);
    const int lat_min = static_cast<int>((lat - lat_deg) * 60.0);
    const int lat_sec = static_cast<int>((lat - lat_deg - lat_min / 60.0) * 3600.0);
    const int lon_deg = static_cast<int>(lon);
    const int lon_min = static_cast<int>((lon - lon_deg) * 60.0);
    const int lon_sec = static_cast<int>((lon - lon_deg - lon_min / 60.0) * 3600.0);

    std::string cmd;
    cmd.reserve(1 + 8);
    cmd.push_back('W');
    cmd.push_back(static_cast<char>(lat_deg));
    cmd.push_back(static_cast<char>(lat_min));
    cmd.push_back(static_cast<char>(lat_sec));
    cmd.push_back(static_cast<char>(lat_sign));
    cmd.push_back(static_cast<char>(lon_deg));
    cmd.push_back(static_cast<char>(lon_min));
    cmd.push_back(static_cast<char>(lon_sec));
    cmd.push_back(static_cast<char>(lon_sign));
    return cmd;
}

}  // namespace alpacacore::util

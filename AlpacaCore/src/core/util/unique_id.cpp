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

#include <alpacacore/util/unique_id.h>

#include <array>
#include <cstdint>
#include <random>

namespace alpacacore::util {

std::string generate_unique_id() {
    std::random_device rd;
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        const std::uint32_t word = rd();
        for (std::size_t j = 0; j < 4; ++j) {
            bytes[i + j] = static_cast<std::uint8_t>(word >> (8 * j));
        }
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);  // RFC 4122 variant
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += kHex[bytes[i] >> 4];
        out += kHex[bytes[i] & 0x0F];
    }
    return out;
}

bool is_unique_id(std::string_view text) {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    const char variant = text[19];
    return text[14] == '4' && (variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
}

}  // namespace alpacacore::util

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

#include <cstdint>
#include <span>
#include <vector>

namespace alpacacore::vendor::celestron {

struct AuxFrame {
    std::uint8_t source = 0;
    std::uint8_t destination = 0;
    std::uint8_t command = 0;
    std::vector<std::uint8_t> payload;
};

/// Encode one complete Celestron AUX frame, including its checksum.
std::vector<std::uint8_t> encode_aux_frame(const AuxFrame& frame);

/// Decode one complete frame. Throws std::invalid_argument for malformed bytes.
AuxFrame decode_aux_frame(std::span<const std::uint8_t> bytes);

}  // namespace alpacacore::vendor::celestron

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

#include <alpacacore/vendor/celestron/celestron_cgx_protocol.h>

#include <algorithm>
#include <stdexcept>

namespace alpacacore::vendor::celestron {

namespace {

constexpr std::uint8_t kAuxStart = 0x3b;
constexpr std::size_t kMinimumFrameSize = 6;
constexpr std::size_t kMaximumFrameSize = 32;

std::uint8_t checksum(std::span<const std::uint8_t> bytes) {
    unsigned int sum = 0;
    for (const std::uint8_t byte : bytes) {
        sum += byte;
    }
    return static_cast<std::uint8_t>(0U - sum);
}

}  // namespace

std::vector<std::uint8_t> encode_aux_frame(const AuxFrame& frame) {
    const std::size_t length = 3 + frame.payload.size();
    const std::size_t frame_size = length + 3;
    if (frame_size > kMaximumFrameSize) {
        throw std::invalid_argument("Celestron AUX payload exceeds the frame limit");
    }

    std::vector<std::uint8_t> bytes(frame_size);
    bytes[0] = kAuxStart;
    bytes[1] = static_cast<std::uint8_t>(length);
    bytes[2] = frame.source;
    bytes[3] = frame.destination;
    bytes[4] = frame.command;
    std::copy(frame.payload.begin(), frame.payload.end(), bytes.begin() + 5);
    bytes.back() = checksum(std::span<const std::uint8_t>(bytes).subspan(1, length + 1));
    return bytes;
}

AuxFrame decode_aux_frame(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kMinimumFrameSize || bytes.size() > kMaximumFrameSize) {
        throw std::invalid_argument("Celestron AUX frame has an invalid size");
    }
    if (bytes[0] != kAuxStart) {
        throw std::invalid_argument("Celestron AUX frame has an invalid start byte");
    }
    const std::size_t length = bytes[1];
    if (length < 3 || length + 3 != bytes.size()) {
        throw std::invalid_argument("Celestron AUX frame length does not match its bytes");
    }
    if (checksum(bytes.subspan(1, bytes.size() - 2)) != bytes.back()) {
        throw std::invalid_argument("Celestron AUX frame checksum is invalid");
    }

    AuxFrame frame;
    frame.source = bytes[2];
    frame.destination = bytes[3];
    frame.command = bytes[4];
    frame.payload.assign(bytes.begin() + 5, bytes.end() - 1);
    return frame;
}

}  // namespace alpacacore::vendor::celestron

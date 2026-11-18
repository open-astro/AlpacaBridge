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

// The NexStar wire helpers the Celestron and SynScan wrappers share (util/nexstar_frame.h, #1741, #1757): table
// cases, no fake, no clock. The wrappers' own fake cases cover the same helpers through the wire.

#include <alpacacore/util/nexstar_frame.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "catch2_compat.h"

using alpacacore::util::build_hex;
using alpacacore::util::leading_hex;
using alpacacore::util::location_command;
using alpacacore::util::parse_axis_pair_response;
using alpacacore::util::parse_mode_byte;
using alpacacore::util::trailing_hex;

// Falsified by: nexstar_frame.h build_hex() dropping std::uppercase, or trailing_hex() stopping at the first
// non-hex byte from the left instead of the right.
TEST_CASE("NexStar frame - hex fields are built upper case and read from either end", "[util][nexstar]") {
    CHECK(build_hex(0xAB, 4) == "00AB");
    CHECK(build_hex(0x123456, 6) == "123456");
    CHECK(trailing_hex("x12AB#") == "12AB");
    CHECK(trailing_hex("12,AB34#") == "AB34");
    CHECK(trailing_hex("#") == "");
    CHECK(leading_hex("#12AB,34") == "12AB");
    CHECK(leading_hex("") == "");
}

// Falsified by: nexstar_frame.h parse_mode_byte() returning the byte value for a digit character too.
TEST_CASE("NexStar frame - a mode byte reads a digit character as its value and any other byte as itself",
          "[util][nexstar]") {
    CHECK(parse_mode_byte("") == 0);
    CHECK(parse_mode_byte("2#") == 2);
    CHECK(parse_mode_byte(std::string("\x02#", 2)) == 2);
    CHECK(parse_mode_byte("A#") == 65);
}

// Falsified by: nexstar_frame.h parse_axis_pair_response() keeping the first expected_digits of the left field
// instead of the field's last eight then their first six (the #785 trim; the Celestron copy before #1757).
TEST_CASE("NexStar frame - an axis pair reads both fields and drops a stale hex byte in front (#785)",
          "[util][nexstar]") {
    using Pair = std::optional<std::pair<uint32_t, uint32_t>>;
    struct Row {
        const char* reply;
        bool precise;
        Pair expected;
    };
    const std::vector<Row> rows = {
        {"1234,ABCD#", false, std::make_pair(0x1234u, 0xABCDu)},
        {"12345600,ABCDEF00#", true, std::make_pair(0x123456u, 0xABCDEFu)},
        // A stale '5' in front of a precise reply: the left field is the last eight digits, never "512345".
        {"512345600,ABCDEF00#", true, std::make_pair(0x123456u, 0xABCDEFu)},
        {"123,ABCD#", false, std::nullopt},  // too few digits on the left
        {"1234,ABC#", false, std::nullopt},  // too few on the right
        {"1234ABCD#", false, std::nullopt},  // no comma
        {"1234,ABCD#", true, std::nullopt},  // a short reply where a precise one is due
    };
    for (const auto& r : rows) {
        INFO("reply " << r.reply << (r.precise ? " (precise)" : ""));
        CHECK(parse_axis_pair_response(r.reply, r.precise) == r.expected);
    }
}

// Falsified by: nexstar_frame.h location_command() putting the sign byte before the degrees, or rounding the
// seconds instead of truncating them.
TEST_CASE("NexStar frame - the location command is W and eight truncated bytes with the signs last",
          "[util][nexstar]") {
    const std::string north_west = location_command(33.5, -117.25);
    CHECK(north_west == std::string{'W', 33, 30, 0, 0, 117, 15, 0, 1});
    const std::string south_east = location_command(-0.5, 10.0);
    CHECK(south_east == std::string{'W', 0, 30, 0, 1, 10, 0, 0, 0});
    // 51.4769 degrees is 51 deg 28 min 36.84 s: the seconds truncate to 36, as both wrappers did.
    const std::string greenwich = location_command(51.4769, -0.0005);
    CHECK(greenwich == std::string{'W', 51, 28, 36, 0, 0, 0, 1, 1});
}

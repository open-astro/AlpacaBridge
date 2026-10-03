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

#include <alpacahttp/wifi_manager.h>

#include <iostream>
#include <string>

#include "test_assert.h"

int main() {
    const std::string arbitrary_bytes{"\xff\0\xfe", 3};
    EXPECT(alpacahttp::util::ssid_to_hex(arbitrary_bytes) == "ff00fe");
    EXPECT(alpacahttp::util::ssid_from_hex("ff") == std::string("\xff", 1));
    EXPECT(alpacahttp::util::ssid_from_hex("FF00Fe") == arbitrary_bytes);
    EXPECT(alpacahttp::util::ssid_from_hex(alpacahttp::util::ssid_to_hex(std::string(32, '\x80'))) ==
           std::string(32, '\x80'));

    for (const auto* malformed : {"", "f", "gg", "000"}) {
        bool rejected = false;
        try {
            (void)alpacahttp::util::ssid_from_hex(malformed);
        } catch (const alpacahttp::util::WifiError&) {
            rejected = true;
        }
        EXPECT(rejected);
    }
    bool too_long_rejected = false;
    try {
        (void)alpacahttp::util::ssid_from_hex(std::string(66, '0'));
    } catch (const alpacahttp::util::WifiError&) {
        too_long_rejected = true;
    }
    EXPECT(too_long_rejected);

    std::cout << "Wi-Fi SSID byte encoding tests passed\n";
    return 0;
}

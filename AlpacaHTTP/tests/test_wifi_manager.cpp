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

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "test_assert.h"

// The 5 GHz country guard runs before any NetworkManager/D-Bus access, so it
// is testable without a wifi device: band "a" with no persisted country must
// be rejected with a message pointing at the regulatory country.
int main() {
    std::cout << "Testing WifiManager 5 GHz country guard...\n";

    const auto dir = std::filesystem::temp_directory_path() / "alpacahttp-wifi-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        alpacahttp::util::WifiManager wm(dir.string());
        bool threw = false;
        try {
            wm.set_ap("TestAP", "password1", "a", 0, true);
        } catch (const alpacahttp::util::WifiError& e) {
            threw = true;
            EXPECT(std::string(e.what()).find("regulatory country") != std::string::npos);
        }
        EXPECT(threw);
    }

    // With a country persisted, band "a" must get past the guard. In a test
    // environment there is no NetworkManager, so any later failure is fine as
    // long as it is not the country message.
    {
        std::ofstream f(dir / "wifi_country");
        f << "US\n";
    }
    {
        alpacahttp::util::WifiManager wm(dir.string());
        try {
            wm.set_ap("TestAP", "password1", "a", 0, true);
        } catch (const alpacahttp::util::WifiError& e) {
            EXPECT(std::string(e.what()).find("regulatory country") == std::string::npos);
        }
    }

    std::filesystem::remove_all(dir);
    std::cout << "All WifiManager tests passed\n";
    return 0;
}

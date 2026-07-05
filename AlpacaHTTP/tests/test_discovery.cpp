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

#include <alpacahttp/config.h>
#include <alpacahttp/discovery.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "test_assert.h"

int main() {
    std::cout << "Testing discovery...\n";

    alpacahttp::Config config;
    config.set_discovery_enabled(true);
    config.set_server_name("TestServer");
    config.set_manufacturer("TestManufacturer");
    config.set_location("TestLocation");

    alpacahttp::Discovery discovery(config);
    
    // Start discovery in background
    discovery.start();
    
    // Give it a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    if (!discovery.is_running()) {
        std::cerr << "Discovery test skipped: unable to bind discovery socket.\n";
        return 0;
    }
    
    // Stop discovery
    discovery.stop();

    EXPECT(!discovery.is_running());

    std::cout << "All discovery tests passed!\n";
    return 0;
}

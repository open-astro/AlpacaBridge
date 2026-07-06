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

#include <iostream>

#include "test_assert.h"

int main() {
    std::cout << "Testing configuration...\n";

    alpacahttp::Config config;

    // Test default values
    EXPECT(config.http_port() == 6800);
    EXPECT(config.discovery_enabled() == true);
    EXPECT(config.log_level() == alpacahttp::LogLevel::WARNING);

    // Test setters
    config.set_http_port(8080);
    EXPECT(config.http_port() == 8080);

    config.set_discovery_enabled(false);
    EXPECT(config.discovery_enabled() == false);

    config.set_log_level(alpacahttp::LogLevel::DEBUG);
    EXPECT(config.log_level() == alpacahttp::LogLevel::DEBUG);

    config.set_server_name("MyServer");
    EXPECT(config.server_name() == "MyServer");

    config.set_manufacturer("MyManufacturer");
    EXPECT(config.manufacturer() == "MyManufacturer");

    config.set_location("MyLocation");
    EXPECT(config.location() == "MyLocation");

    // Test device enable/disable (default should be enabled)
    EXPECT(config.is_device_enabled("camera", 0) == true);

    std::cout << "All configuration tests passed!\n";
    return 0;
}

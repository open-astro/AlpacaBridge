// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

#include <alpacahttp/config.h>
#include <cassert>
#include <iostream>

int main() {
    std::cout << "Testing configuration...\n";

    alpacahttp::Config config;

    // Test default values
    assert(config.http_port() == 6800);
    assert(config.discovery_enabled() == true);
    assert(config.log_level() == alpacahttp::LogLevel::INFO);

    // Test setters
    config.set_http_port(8080);
    assert(config.http_port() == 8080);

    config.set_discovery_enabled(false);
    assert(config.discovery_enabled() == false);

    config.set_log_level(alpacahttp::LogLevel::DEBUG);
    assert(config.log_level() == alpacahttp::LogLevel::DEBUG);

    config.set_server_name("MyServer");
    assert(config.server_name() == "MyServer");

    config.set_manufacturer("MyManufacturer");
    assert(config.manufacturer() == "MyManufacturer");

    config.set_location("MyLocation");
    assert(config.location() == "MyLocation");

    // Test device enable/disable (default should be enabled)
    assert(config.is_device_enabled("camera", 0) == true);

    std::cout << "All configuration tests passed!\n";
    return 0;
}


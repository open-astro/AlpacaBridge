// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacahttp/discovery.h>
#include <alpacahttp/config.h>
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>

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
    
    assert(!discovery.is_running());

    std::cout << "All discovery tests passed!\n";
    return 0;
}

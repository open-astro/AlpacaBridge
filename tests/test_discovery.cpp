// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

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
    
    assert(discovery.is_running());
    
    // Stop discovery
    discovery.stop();
    
    assert(!discovery.is_running());

    std::cout << "All discovery tests passed!\n";
    return 0;
}


// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://github.com/open-astro/AlpacaHTTP/blob/main/LICENSE
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

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


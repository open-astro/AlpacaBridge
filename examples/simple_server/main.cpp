// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

#include <alpacahttp/server.h>
#include <alpacahttp/config.h>
#include <alpacahttp/discovery.h>
#include <alpacahttp/util/logging_adapter.h>
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>

std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    (void)signal;
    g_running = false;
}

int main(int argc, char* argv[]) {
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Load configuration
    alpacahttp::Config config;
    if (argc > 1) {
        config.load(argv[1]);
    } else {
        config.load_default();
    }

    // Initialize logging
    // TODO: Connect to AlpacaCore logging system
    init_logging(config, [](int level, const std::string& message) {
        const char* level_str = "INFO";
        switch (level) {
            case 0: level_str = "DEBUG"; break;
            case 1: level_str = "INFO"; break;
            case 2: level_str = "WARNING"; break;
            case 3: level_str = "ERROR"; break;
        }
        std::cout << "[" << level_str << "] " << message << std::endl;
    });

    log_info("Starting AlpacaHTTP server...");
    log_info("HTTP port: " + std::to_string(config.http_port()));
    log_info("Discovery enabled: " + std::string(config.discovery_enabled() ? "yes" : "no"));

    // Start discovery service
    std::unique_ptr<alpacahttp::Discovery> discovery;
    if (config.discovery_enabled()) {
        discovery = std::make_unique<alpacahttp::Discovery>(config);
        discovery->start();
        log_info("Discovery service started");
    }

    // Start HTTP server
    alpacahttp::Server server(config);
    server.start_async();

    // Wait for shutdown signal
    while (g_running && server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    log_info("Shutting down...");

    // Stop services
    server.stop();
    if (discovery) {
        discovery->stop();
    }

    log_info("Server stopped");
    return 0;
}


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
#include <alpacacore/device_registry.h>
#include <alpacacore/managementdriver.h>
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

    // Initialize logging - connect to AlpacaCore logging system
    init_logging(config);
    
    // Optionally set a custom log sink
    alpacacore::logging::set_log_sink([](alpacacore::logging::LogLevel level,
                                         std::string_view component,
                                         std::string_view message) {
        const char* level_str = "INFO";
        switch (level) {
            case alpacacore::logging::LogLevel::Trace:
            case alpacacore::logging::LogLevel::Debug: level_str = "DEBUG"; break;
            case alpacacore::logging::LogLevel::Info: level_str = "INFO"; break;
            case alpacacore::logging::LogLevel::Warn: level_str = "WARNING"; break;
            case alpacacore::logging::LogLevel::Error:
            case alpacacore::logging::LogLevel::Critical: level_str = "ERROR"; break;
        }
        std::cout << "[" << level_str << "] [" << component << "] " << message << std::endl;
    });

    log_info("Starting AlpacaHTTP server...");
    log_info("HTTP port: " + std::to_string(config.http_port()));
    log_info("Discovery enabled: " + std::string(config.discovery_enabled() ? "yes" : "no"));

    // TODO: Register devices with AlpacaCore DeviceRegistry
    // Example:
    // auto& registry = alpacacore::management::DeviceRegistry::instance();
    // auto camera = std::make_shared<YourCameraDriver>(...);
    // registry.register_device(camera);
    
    // TODO: Create and set ManagementDriver
    // Example:
    // auto mgmt_driver = std::make_shared<YourManagementDriver>(config);
    // server.set_management_driver(mgmt_driver);

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


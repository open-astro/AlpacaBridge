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

#include <alpacahttp/server.h>
#include <alpacahttp/config.h>
#include <alpacahttp/discovery.h>
#include <alpacahttp/util/logging_adapter.h>
#include <alpacacore/device_registry.h>
#include <alpacacore/managementdriver.h>
#include <alpacacore/vendor/touptek/touptek_filterwheel_driver.h>
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>
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
    alpacahttp::util::init_logging(config);
    
    // Optionally set a custom log sink
    alpacahttp::util::set_external_log_sink([](alpacacore::logging::LogLevel level,
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

    alpacahttp::util::log_info("Starting AlpacaHTTP server...");
    alpacahttp::util::log_info("HTTP port: " + std::to_string(config.http_port()));
    alpacahttp::util::log_info("Discovery enabled: " + std::string(config.discovery_enabled() ? "yes" : "no"));

    // Register ToupTek camera + filter wheel devices
    auto& registry = alpacacore::management::DeviceRegistry::instance();
    auto camera = alpacacore::vendor::touptek::create_touptek_camera(0, 0);
    registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(camera.release()));
    auto filterwheel = alpacacore::vendor::touptek::create_touptek_filterwheel(0, 0);
    registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(filterwheel.release()));

    // Start discovery service
    std::unique_ptr<alpacahttp::Discovery> discovery;
    if (config.discovery_enabled()) {
        discovery = std::make_unique<alpacahttp::Discovery>(config);
        discovery->start();
        alpacahttp::util::log_info("Discovery service started");
    }

    // Start HTTP server
    alpacahttp::Server server(config);
    
    // Set shutdown callback - when shutdown endpoint is called, set g_running to false
    server.set_shutdown_callback([]() {
        g_running = false;
    });
    
    server.start_async();

    // Wait for shutdown signal
    while (g_running && server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    alpacahttp::util::log_info("Shutting down...");

    // Stop services in reverse order of startup
    if (discovery) {
        discovery->stop();
    }
    server.stop();

    // Give threads a moment to fully exit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    alpacahttp::util::log_info("Server stopped");
    
    // Flush any remaining log output
    std::cout.flush();
    std::cerr.flush();
    
    // Use exit() to ensure process terminates even if there are lingering threads
    // This is safe since we've already stopped all services
    exit(0);
}

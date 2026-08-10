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

#include <alpacacore/device_registry.h>
#include <alpacacore/managementdriver.h>
#include <alpacahttp/config.h>
#include <alpacahttp/discovery.h>
#include <alpacahttp/server.h>
#include <alpacahttp/util/logging_adapter.h>
#include <alpacahttp/wifi_manager.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

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

    // Re-apply the persisted WiFi regulatory country BEFORE anything else
    // brings the radio up. NetworkManager autoconnects the hotspot profile
    // during boot, and some drivers (iMate unisoc_wifi) refuse 5 GHz AP init
    // under the default WORLD regdom - applying lazily on the first wifi
    // HTTP request would lose that race (PR #198 review). No-op when no
    // country has been persisted.
    alpacahttp::util::WifiManager("config").apply_persisted_country();

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

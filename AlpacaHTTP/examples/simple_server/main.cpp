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
#include <alpacacore/version.h>
#include <alpacahttp/config.h>
#include <alpacahttp/discovery.h>
#include <alpacahttp/server.h>
#include <alpacahttp/util/logging_adapter.h>
#include <alpacahttp/wifi_manager.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

std::atomic<bool> g_running{true};

namespace {

void print_usage(std::ostream& out) {
    out << "Usage: alpacabridge [--help | -h | --version] [config-file]\n"
           "\n"
           "Runs the AlpacaBridge server. With no argument it loads the default\n"
           "configuration; with a config-file argument it loads that file, which must be readable.\n"
           "\n"
           "Options:\n"
           "  -h, --help     Print this usage text and exit.\n"
           "  --version      Print the version and exit.\n"
           "\n"
           "Environment overrides (applied over the configuration):\n"
           "  ALPACAHTTP_PORT\n"
           "  ALPACAHTTP_DISCOVERY\n"
           "  ALPACAHTTP_DISCOVERY_PORT\n"
           "  ALPACAHTTP_LOG_LEVEL\n"
           "  ALPACAHTTP_SERVER_NAME\n"
           "  ALPACAHTTP_MANUFACTURER\n"
           "  ALPACAHTTP_LOCATION\n"
           "  ALPACAHTTP_PROFILE_NAME\n"
           "  ALPACAHTTP_THREAD_POOL_SIZE\n"
           "  ALPACAHTTP_MAX_CONNECTIONS\n"
           "  ALPACAHTTP_KEEP_ALIVE_LIFETIME_SECONDS\n"
           "  ALPACAHTTP_MOTION_WATCHDOG_SECONDS\n"
           "  ALPACAHTTP_UPDATE_PACKAGES_URL\n"
           "  ALPACAHTTP_UPDATE_RELEASE_NOTES_URL\n"
           "  ALPACAHTTP_UPDATE_RELEASE_URL\n"
           "  ALPACAHTTP_LOG_DIRECTORY\n"
           "  ALPACAHTTP_FILE_LOGGING\n"
           "  ALPACAHTTP_LOG_RETENTION_DAYS\n"
           "  ALPACAHTTP_LOG_MAX_FILE_SIZE_MB\n";
}

// Handles the arguments that must not start a server. Returns the exit status to
// leave with, or -1 to carry on. Runs before the config load, logging, Wi-Fi and
// any socket, so a mistyped command line touches nothing.
int check_arguments(int argc, char* argv[]) {
    if (argc < 2) {
        return -1;
    }
    const std::string first = argv[1];
    if (first == "--help" || first == "-h") {
        print_usage(std::cout);
        return 0;
    }
    if (first == "--version") {
        std::cout << alpacacore::kVersion << std::endl;
        return 0;
    }
    if (!first.empty() && first[0] == '-') {
        std::cerr << "alpacabridge: unknown option '" << first << "'\n";
        print_usage(std::cerr);
        return 2;
    }
    if (argc > 2) {
        std::cerr << "alpacabridge: expected at most one config-file argument, got " << (argc - 1) << "\n";
        print_usage(std::cerr);
        return 2;
    }
    // Config::load() skips a file it cannot open and still succeeds, so a mistyped
    // path would start a server on defaults. Refuse it here instead.
    std::error_code ec;
    if (std::filesystem::is_directory(first, ec) || !std::ifstream(first).is_open()) {
        std::cerr << "alpacabridge: cannot read config file '" << first << "'\n";
        return 1;
    }
    return -1;
}

}  // namespace

void signal_handler(int signal) {
    (void)signal;
    g_running = false;
}

int main(int argc, char* argv[]) {
    if (const int status = check_arguments(argc, argv); status >= 0) {
        return status;
    }

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

    // With http_port 0 the OS picks the port, and discovery stays silent
    // until it is told which one (#562). bound_port() reads 0 until the
    // listener is up, so keep trying from the wait loop below. A management
    // restart binds a new ephemeral port, so remember the last one advertised
    // and advertise again when it changes (#761). While the restart has the
    // listener closed bound_port() reads 0 again: clear the advertised port so
    // discovery stays silent instead of pointing clients at a closed port, and
    // advertise the new one once it binds (#889).
    const bool track_port = discovery && config.http_port() == 0;
    std::uint16_t advertised_port = 0;

    // Wait for shutdown signal
    while (g_running && server.is_running()) {
        if (track_port) {
            const std::uint16_t port = server.bound_port();
            if (port == 0 && advertised_port != 0) {
                discovery->set_advertised_port(0);
                alpacahttp::util::log_info("Discovery silent: HTTP listener is closed");
                advertised_port = 0;
            } else if (port != 0 && port != advertised_port) {
                discovery->set_advertised_port(port);
                alpacahttp::util::log_info("Discovery advertising HTTP port " + std::to_string(port));
                advertised_port = port;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // A loop that ended with g_running still true was ended by the server
    // itself (a restart whose listener could not re-bind, a failed start).
    // Nobody asked for this, so exit 1: systemd's Restart=on-failure respawns
    // the service, where a 0 would have left it down for good (#713).
    const bool stop_requested = !g_running;
    int exit_status = 0;
    if (!stop_requested) {
        alpacahttp::util::log_error(
            "HTTP server stopped without a shutdown request (restart could not re-bind the listener, or the "
            "listener failed); exiting with status 1 so systemd restarts the service");
        exit_status = 1;
    }

    alpacahttp::util::log_info("Shutting down...");

    // Stop services in reverse order of startup
    if (discovery) {
        discovery->stop();
    }
    server.stop();

    // Give threads a moment to fully exit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Destroy registered devices BEFORE exiting: driver destructors cancel and
    // JOIN their async task threads (slew/park/home/pulse) and stop the motors.
    // Skipping this and calling exit() ran static destructors (including the
    // protocol wrapper singletons) under still-running task threads — observed
    // as a SIGSEGV on systemctl stop while a SkyWatcher AutoHome was in flight.
    alpacacore::management::DeviceRegistry::instance().clear();

    alpacahttp::util::log_info("Server stopped");

    // Flush any remaining log output
    std::cout.flush();
    std::cerr.flush();

    // Use exit() to ensure process terminates even if there are lingering threads
    // (all device task threads were joined by the registry clear above).
    exit(exit_status);
}

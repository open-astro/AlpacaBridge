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

// Test-only server for the hardware-free ConformU job (scripts/run_fake_conformu.sh).
// It serves one Telescope through a test catalog descriptor
// ("skywatcher-fake") whose factory builds the real Sky-Watcher driver over
// FakeSkyWatcherMount's loopback UDP port. The run script registers device 0
// with POST /management/v1/configuredevice (the router's own path). Not installed or packaged, and not
// linked into alpacabridge or alpacahttp_server. A ConformU run against it is
// fake evidence only: it says nothing about hardware.
//
// The descriptor reaches the catalog through Server::router_for_test(), the same
// seam the catalog tests use. Everything the Schema borrows has static storage
// (schema.h): the display name and build option are string literals and the
// field span is empty.

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/catalog/device_catalog.h>
#include <alpacahttp/config.h>
#include <alpacahttp/server.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "conformu_skywatcher_factory.h"

namespace {

namespace catalog = alpacacore::catalog;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

// Static storage: the factory's mount outlives every driver it builds.
alpacacore::test::FakeSkyWatcherMount& fake_mount() {
    static alpacacore::test::FakeSkyWatcherMount mount;
    return mount;
}

catalog::Schema make_schema() {
    catalog::Schema schema;
    schema.key = catalog::DeviceKey{"skywatcher-fake", alpacacore::DeviceType::Telescope};
    schema.display_name = "Sky-Watcher (fake mount, test only)";
    schema.build_option = "ALPACACORE_ENABLE_SKYWATCHER";
    return schema;  // fields: empty span
}

catalog::Factory make_factory() {
    catalog::Factory factory;
    factory.key = catalog::DeviceKey{"skywatcher-fake", alpacacore::DeviceType::Telescope};
    factory.create = [](const catalog::DeviceConfig&, int device_number) -> std::unique_ptr<alpacacore::AlpacaDriver> {
        return alpacacore::test::make_fake_skywatcher_telescope(device_number, fake_mount());
    };
    return factory;
}

}  // namespace

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 11111;
    if (port <= 0 || port > 65535) {
        std::cerr << "usage: fake_skywatcher_server [http-port]\n";
        return 2;
    }
    if (!fake_mount().ok()) {
        std::cerr << "fake mount failed to bind its UDP port\n";
        return 1;
    }

    alpacahttp::Config config;
    config.set_http_port(static_cast<std::uint16_t>(port));
    config.set_discovery_enabled(false);
    config.set_server_name("FakeSkyWatcher");

    alpacahttp::Server server(config);
    auto& router = server.router_for_test();
    router.catalog().add(make_schema());
    router.catalog().add(make_factory());

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    server.start_async();
    std::cout << "fake Sky-Watcher server on port " << port << std::endl;
    while (g_stop == 0 && server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.stop();
    return 0;
}

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

// Test-only server for the hardware-free ConformU job (scripts/run_fake_conformu.sh, issue #2078).
// It serves one device: the row of scripts/conformu_fake_runs.tsv named on its command line, through a test
// catalog descriptor ("<vendor>-fake") whose factory builds the real driver over that row's fake. The hosts are
// the table below; a row the table does not know is refused, so the run list cannot ask for a host this server
// cannot build. The run script registers device 0 with POST /management/v1/configuredevice (the router's own
// path). Not installed or packaged, and not linked into alpacabridge or alpacahttp_server. A ConformU run against
// it is fake evidence only: it says nothing about hardware.
//
// The descriptor reaches the catalog through Server::router_for_test(), the same seam the catalog tests use.
// Everything the Schema borrows has static storage (schema.h): the display name, the build option and the vendor
// key are string literals in the host table, and the field span is empty.
//
//   fake_conformu_server <http-port> <vendor> <deviceType> <fakeHeader>
//   fake_conformu_server --describe <vendor> <deviceType> <fakeHeader>
//
// --describe prints the fake's description, which the run script puts in the report's FAKE REPORT line.

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/catalog/device_catalog.h>
#include <alpacahttp/config.h>
#include <alpacahttp/server.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "conformu_skywatcher_factory.h"

namespace {

namespace catalog = alpacacore::catalog;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

// One ConformU host: a run-list row (vendor, deviceType, fakeHeader), the descriptor it registers and how to build
// its driver over the fake. `ready` reports whether the fake came up (a bound port, an open pty).
struct ConformuHost {
    std::string_view vendor;
    std::string_view device_type;
    std::string_view fake_header;
    alpacacore::DeviceType type;
    const char* descriptor_vendor;  // the catalog vendor key the run script registers
    const char* display_name;
    const char* build_option;
    const char* description;  // the FAKE REPORT line's "ConformU against ..." text
    std::function<bool()> ready;
    std::function<std::unique_ptr<alpacacore::AlpacaDriver>(int device_number)> make;
};

// Static storage: the fake outlives every driver the factory builds. Each fake is built only when its row is served.
alpacacore::test::FakeSkyWatcherMount& skywatcher_mount() {
    static alpacacore::test::FakeSkyWatcherMount mount;
    return mount;
}

alpacacore::test::FakeSkyWatcherSerialBoard& skywatcher_serial_board() {
    static alpacacore::test::FakeSkyWatcherSerialBoard board;
    return board;
}

const ConformuHost kHosts[] = {
    {"skywatcher", "telescope", "fake_skywatcher_mount.h", alpacacore::DeviceType::Telescope, "skywatcher-fake",
     "Sky-Watcher (fake mount, test only)", "ALPACACORE_ENABLE_SKYWATCHER",
     "FakeSkyWatcherMount (simulated mount over loopback UDP)", [] { return skywatcher_mount().ok(); },
     [](int device_number) -> std::unique_ptr<alpacacore::AlpacaDriver> {
         return alpacacore::test::make_fake_skywatcher_telescope(device_number, skywatcher_mount());
     }},
    {"skywatcher", "telescope", "fake_skywatcher_serial_board.h", alpacacore::DeviceType::Telescope, "skywatcher-fake",
     "Sky-Watcher (fake serial motor board, test only)", "ALPACACORE_ENABLE_SKYWATCHER",
     "FakeSkyWatcherSerialBoard (simulated motor board over a pseudo-terminal)",
     [] { return !skywatcher_serial_board().slave_path().empty(); },
     [](int device_number) -> std::unique_ptr<alpacacore::AlpacaDriver> {
         return alpacacore::test::make_fake_skywatcher_serial_telescope(device_number, skywatcher_serial_board());
     }},
};

const ConformuHost* find_host(std::string_view vendor, std::string_view device_type, std::string_view fake_header) {
    for (const auto& host : kHosts) {
        if (host.vendor == vendor && host.device_type == device_type && host.fake_header == fake_header) return &host;
    }
    return nullptr;
}

catalog::Schema make_schema(const ConformuHost& host) {
    catalog::Schema schema;
    schema.key = catalog::DeviceKey{host.descriptor_vendor, host.type};
    schema.display_name = host.display_name;
    schema.build_option = host.build_option;
    return schema;  // fields: empty span
}

catalog::Factory make_factory(const ConformuHost& host) {
    catalog::Factory factory;
    factory.key = catalog::DeviceKey{host.descriptor_vendor, host.type};
    factory.create = [&host](const catalog::DeviceConfig&,
                             int device_number) -> std::unique_ptr<alpacacore::AlpacaDriver> {
        return host.make(device_number);
    };
    return factory;
}

int usage() {
    std::cerr << "usage: fake_conformu_server <http-port> <vendor> <deviceType> <fakeHeader>\n"
                 "       fake_conformu_server --describe <vendor> <deviceType> <fakeHeader>\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) return usage();
    const std::string_view first = argv[1];
    const ConformuHost* host = find_host(argv[2], argv[3], argv[4]);
    if (host == nullptr) {
        std::cerr << "no ConformU host for {" << argv[2] << ", " << argv[3] << ", " << argv[4]
                  << "}: add one to kHosts in AlpacaHTTP/tests/fake_conformu_server.cpp\n";
        return 2;
    }
    if (first == "--describe") {
        std::cout << host->description << std::endl;
        return 0;
    }
    const int port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) return usage();
    if (!host->ready()) {
        std::cerr << "the fake for {" << argv[2] << ", " << argv[3] << ", " << argv[4] << "} did not start\n";
        return 1;
    }

    alpacahttp::Config config;
    config.set_http_port(static_cast<std::uint16_t>(port));
    config.set_discovery_enabled(false);
    config.set_server_name("FakeConformU");

    alpacahttp::Server server(config);
    auto& router = server.router_for_test();
    router.catalog().add(make_schema(*host));
    router.catalog().add(make_factory(*host));

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    server.start_async();
    std::cout << "fake ConformU server for " << argv[2] << " " << argv[3] << " on port " << port << std::endl;
    while (g_stop == 0 && server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.stop();
    return 0;
}

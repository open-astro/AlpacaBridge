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

// Concurrency test for the router's persisted_devices_ store (audit 3.0.1
// follow-up). The real server dispatches each HTTP request on its own thread,
// so handle_configure_device / handle_remove_device / handle_configured_devices
// can all mutate or iterate persisted_devices_ concurrently — before the
// persisted_devices_mutex_ guard this was a data race on a std::vector
// (concurrent push_back / erase / iteration -> UB, often a crash). This is a
// plain threaded test: it storms Router::route from many threads and asserts
// nothing crashes and the final state is coherent. Run it under TSan for race
// detection; without TSan it still catches crashes and corruption.

#include <alpacahttp/request.h>
#include <alpacahttp/router.h>

#include <atomic>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "test_assert.h"

namespace {

alpacahttp::Response route_request(alpacahttp::Router& router, const std::string& method, const std::string& path,
                                   const std::string& body = std::string()) {
    alpacahttp::Request request;
    std::ostringstream raw;
    raw << method << " " << path << " HTTP/1.1\r\n";
    raw << "Host: localhost\r\n";
    if (!body.empty()) {
        raw << "Content-Type: application/json\r\n";
        raw << "Content-Length: " << body.size() << "\r\n";
    }
    raw << "\r\n";
    raw << body;

    EXPECT(request.parse(raw.str()));
    return router.route(request, 1);
}

// Hardware-free config: Celestron telescope registration opens nothing at
// configure time (the async connect only happens on Connected=true), so the
// full configure -> persist -> remove path runs on any host.
nlohmann::json configure_body(int device_number) {
    return nlohmann::json{{"vendor", "celestron"},      {"deviceType", "telescope"}, {"deviceNumber", device_number},
                          {"connectionType", "serial"}, {"portPath", "/dev/null"},   {"baudRate", 9600},
                          {"responseTimeoutMs", 5000}};
}

nlohmann::json remove_body(int device_number) {
    return nlohmann::json{{"vendor", "celestron"}, {"deviceType", "telescope"}, {"deviceNumber", device_number}};
}

}  // namespace

int main() {
#ifndef ALPACACORE_ENABLE_CELESTRON
    std::cout << "Celestron vendor disabled; skipping persisted-devices concurrency test.\n";
    return 0;
#else
    std::cout << "Testing persisted-devices concurrency...\n";

    alpacahttp::Router router;

    // Device numbers deliberately OVERLAP between threads (two writer threads
    // per number) so add_or_replace and remove contend on the SAME persisted
    // entries, not just on the vector's tail.
    constexpr int kWriterThreads = 4;
    constexpr int kReaderThreads = 4;
    constexpr int kIterations = 40;
    constexpr int kBaseNumber = 9400;
    constexpr int kNumbersPerThread = 3;

    // Pre-clean from any previous run (the store persists to config/).
    for (int n = kBaseNumber; n < kBaseNumber + (kWriterThreads / 2) * kNumbersPerThread; ++n) {
        static_cast<void>(route_request(router, "POST", "/management/v1/removedevice", remove_body(n).dump()));
    }

    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    threads.reserve(kWriterThreads + kReaderThreads);

    for (int t = 0; t < kWriterThreads; ++t) {
        threads.emplace_back([&router, &failed, t]() {
            // Threads t and t + kWriterThreads/2 share the same numbers.
            const int base = kBaseNumber + (t % (kWriterThreads / 2)) * kNumbersPerThread;
            try {
                for (int i = 0; i < kIterations; ++i) {
                    const int number = base + (i % kNumbersPerThread);
                    static_cast<void>(
                        route_request(router, "POST", "/management/v1/configuredevice", configure_body(number).dump()));
                    static_cast<void>(
                        route_request(router, "POST", "/management/v1/removedevice", remove_body(number).dump()));
                }
            } catch (const std::exception& e) {
                std::cerr << "writer thread threw: " << e.what() << "\n";
                failed.store(true);
            }
        });
    }
    for (int t = 0; t < kReaderThreads; ++t) {
        threads.emplace_back([&router, &failed]() {
            try {
                for (int i = 0; i < kIterations; ++i) {
                    const auto response = route_request(router, "GET", "/management/v1/configureddevices");
                    // The reader iterates the persisted store; the body must
                    // always be a parseable success envelope.
                    const auto json = nlohmann::json::parse(response.body(), nullptr, false);
                    if (json.is_discarded() || json.value("ErrorNumber", -1) != 0) {
                        std::cerr << "configureddevices returned an incoherent body\n";
                        failed.store(true);
                        return;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "reader thread threw: " << e.what() << "\n";
                failed.store(true);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    EXPECT(!failed.load());

    // Post-storm coherence: remove every number (idempotent), then the
    // persisted list must contain none of them.
    for (int n = kBaseNumber; n < kBaseNumber + (kWriterThreads / 2) * kNumbersPerThread; ++n) {
        static_cast<void>(route_request(router, "POST", "/management/v1/removedevice", remove_body(n).dump()));
    }
    const auto response = route_request(router, "GET", "/management/v1/configureddevices");
    const auto json = nlohmann::json::parse(response.body());
    EXPECT(json.value("ErrorNumber", -1) == 0);
    EXPECT(json.contains("Value") && json["Value"].is_array());
    for (const auto& entry : json["Value"]) {
        const int number = entry.value("DeviceNumber", -1);
        EXPECT(!(number >= kBaseNumber && number < kBaseNumber + (kWriterThreads / 2) * kNumbersPerThread));
    }

    std::cout << "All persisted-devices concurrency tests passed!\n";
    return 0;
#endif
}

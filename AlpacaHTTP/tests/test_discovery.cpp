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

#include <alpacacore/util/logging.h>
#include <alpacahttp/config.h>
#include <alpacahttp/discovery.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "test_assert.h"

int main() {
    std::cout << "Testing discovery...\n";

    alpacahttp::Config config;
    config.set_discovery_enabled(true);
    config.set_server_name("TestServer");
    config.set_manufacturer("TestManufacturer");
    config.set_location("TestLocation");

    // open-astro#740: a datagram that is not an Alpaca probe is client
    // input, so the line it produces is DEBUG, not WARNING. The level is
    // lowered to Debug so the line is observable, and the sink captures
    // every level so the case can wait for the line before asserting.
    struct CapturedLine {
        alpacacore::logging::LogLevel level;
        std::string message;
    };
    std::vector<CapturedLine> captured;
    std::mutex captured_mutex;
    struct LoggingRestore {
        alpacacore::logging::LogLevel level = alpacacore::logging::get_log_level();
        alpacacore::logging::LogSink sink = alpacacore::logging::get_log_sink();
        ~LoggingRestore() {
            alpacacore::logging::set_log_sink(sink);
            alpacacore::logging::set_log_level(level);
        }
    } logging_restore;
    alpacacore::logging::set_log_level(alpacacore::logging::LogLevel::Debug);
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            std::lock_guard<std::mutex> lock(captured_mutex);
            captured.push_back({level, std::string(message)});
        });

    alpacahttp::Discovery discovery(config);
    
    // Start discovery in background
    discovery.start();

    {
        // is_running() is set before the listener thread binds, so a datagram
        // sent on it alone can arrive at no socket. The thread logs this line
        // only after bind() and the multicast join, so wait for it instead.
        const std::string started = "Discovery service started on port ";
        bool listening = false;
        for (int i = 0; i < 100 && !listening && discovery.is_running(); ++i) {
            {
                std::lock_guard<std::mutex> lock(captured_mutex);
                for (const auto& line : captured) {
                    if (line.message.rfind(started, 0) == 0) {
                        listening = true;
                    }
                }
            }
            if (!listening) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (!discovery.is_running()) {
            std::cerr << "Discovery test skipped: unable to bind discovery socket.\n";
            return 0;
        }
        EXPECT(listening);

        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        EXPECT(fd >= 0);
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        target.sin_port = htons(32227);  // the Alpaca discovery port
        const char payload[] = "hello";
        EXPECT(sendto(fd, payload, std::strlen(payload), 0, reinterpret_cast<sockaddr*>(&target), sizeof(target)) ==
               static_cast<ssize_t>(std::strlen(payload)));
        close(fd);

        const std::string tag = "Discovery: Received non-Alpaca probe from ";
        bool seen = false;
        alpacacore::logging::LogLevel seen_level = alpacacore::logging::LogLevel::Warn;
        for (int i = 0; i < 50 && !seen; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::lock_guard<std::mutex> lock(captured_mutex);
            for (const auto& line : captured) {
                if (line.message.find(tag) != std::string::npos) {
                    seen = true;
                    seen_level = line.level;
                }
            }
        }
        EXPECT(seen);
        // Exactly DEBUG: INFO or ERROR would still put client input in the log.
        EXPECT(seen_level == alpacacore::logging::LogLevel::Debug);
    }

    // Stop discovery
    discovery.stop();

    {
        std::lock_guard<std::mutex> lock(captured_mutex);
        for (const auto& line : captured) {
            // The multicast join warning depends on the host's routes, not
            // on anything a client sent.
            if (line.message.rfind("Failed to join Alpaca multicast group", 0) == 0) {
                continue;
            }
            EXPECT(line.level != alpacacore::logging::LogLevel::Warn);
        }
    }

    EXPECT(!discovery.is_running());

    std::cout << "All discovery tests passed!\n";
    return 0;
}

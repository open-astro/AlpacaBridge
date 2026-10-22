// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#ifndef _WIN32

#include <alpacacore/vendor/zwo/zwo_mount_protocol_wrapper.h>

#include <barrier>
#include <exception>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_mount_server.h"

namespace {

using alpacacore::test::FakeMountServer;
using alpacacore::vendor::zwo::ConnectionInfo;
using alpacacore::vendor::zwo::ConnectionType;
using alpacacore::vendor::zwo::ZWOMountProtocolWrapper;

ConnectionInfo network_info(const std::string& host, int port) {
    ConnectionInfo info;
    info.type = ConnectionType::Network;
    info.host = host;
    info.tcp_port = port;
    info.response_timeout_ms = 500;
    return info;
}

}  // namespace

TEST_CASE("ZWO mount network - concurrent cold connects retain distinct endpoints",
          "[zwo][network][isolation][issue1524]") {
    FakeMountServer first([](const std::string&) { return std::string("AM5#"); });
    FakeMountServer second([](const std::string&) { return std::string("AM5N#"); }, INADDR_LOOPBACK + 1);  // 127.0.0.2
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    const ConnectionInfo infos[] = {network_info("127.0.0.1", first.port()), network_info("127.0.0.2", second.port())};

    // Distinct hosts catch a resolver result borrowed from a sibling; ports alone do not.
    // shortcut: repetition cannot force the libc race; add deterministic coverage if a resolver seam is introduced.
    for (int iteration = 0; iteration < 64; ++iteration) {
        INFO("cold connect iteration " << iteration);
        ZWOMountProtocolWrapper wrappers[2];
        std::barrier start(2);
        bool connected[2] = {false, false};
        std::exception_ptr errors[2];
        const auto connect = [&](int index) {
            start.arrive_and_wait();
            try {
                connected[index] = wrappers[index].connect(infos[index]);
            } catch (...) {
                errors[index] = std::current_exception();
            }
        };
        std::jthread a(connect, 0);
        std::jthread b(connect, 1);
        a.join();
        b.join();
        for (int index = 0; index < 2; ++index) {
            if (errors[index]) {
                std::rethrow_exception(errors[index]);
            }
            REQUIRE(connected[index]);
            CHECK(wrappers[index].is_connected());
        }
        CHECK(wrappers[0].get_mount_info() == "AM5");
        CHECK(wrappers[1].get_mount_info() == "AM5N");
    }
}

TEST_CASE("ZWO mount network - invalid hostname cleanup preserves a sibling and permits retry",
          "[zwo][network][isolation][issue1524]") {
    FakeMountServer first([](const std::string&) { return std::string("AM5#"); });
    FakeMountServer second([](const std::string&) { return std::string("AM5N#"); }, INADDR_LOOPBACK + 1);  // 127.0.0.2
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    ZWOMountProtocolWrapper sibling;
    REQUIRE(sibling.connect(network_info("127.0.0.1", first.port())));
    REQUIRE(sibling.get_mount_info() == "AM5");
    {
        ZWOMountProtocolWrapper retry;
        CHECK_FALSE(retry.connect(network_info("issue1524-mount.invalid", second.port())));
        CHECK_FALSE(retry.is_connected());
        retry.disconnect();
        CHECK(sibling.is_connected());
        CHECK(sibling.get_mount_info() == "AM5");

        REQUIRE(retry.connect(network_info("127.0.0.2", second.port())));
        CHECK(retry.get_mount_info() == "AM5N");
        CHECK(sibling.get_mount_info() == "AM5");
    }
    CHECK(sibling.is_connected());
    CHECK(sibling.get_mount_info() == "AM5");
}

#endif  // !_WIN32

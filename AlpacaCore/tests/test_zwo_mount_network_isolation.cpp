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
#include <future>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_mount_server.h"

namespace {

using alpacacore::test::FakeMountServer;
using alpacacore::vendor::zwo::ConnectionInfo;
using alpacacore::vendor::zwo::ConnectionType;
using alpacacore::vendor::zwo::enumerate_zwo_mounts;
using alpacacore::vendor::zwo::ZWODeviceInfo;
using alpacacore::vendor::zwo::ZWOMountProtocolWrapper;

ConnectionInfo network_info(const std::string& host, int port) {
    ConnectionInfo info;
    info.type = ConnectionType::Network;
    info.host = host;
    info.tcp_port = port;
    info.response_timeout_ms = 500;
    return info;
}

std::vector<ZWODeviceInfo> candidates(const FakeMountServer& server, const std::string& host = "127.0.0.1") {
    return {{ConnectionType::Network, "", host, server.port(), "", "loopback"}};
}

ConnectionInfo auto_info() {
    ConnectionInfo info;
    info.type = ConnectionType::Auto;
    info.response_timeout_ms = 1500;
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

TEST_CASE("ZWO mount Auto - skips a held endpoint before probing and reaches a free port",
          "[zwo][network][auto][issue1536]") {
    FakeMountServer held([](const std::string&) { return std::string("AM5#"); });
    FakeMountServer free([](const std::string&) { return std::string("AM7#"); });
    REQUIRE(held.ok());
    REQUIRE(free.ok());
    ZWOMountProtocolWrapper owner;
    REQUIRE(owner.connect(network_info("localhost", held.port())));
    REQUIRE(owner.get_mount_info() == "AM5");
    auto inputs = candidates(held);
    inputs.push_back(candidates(free).front());
    ZWOMountProtocolWrapper automatic(false, inputs);
    REQUIRE(automatic.connect(auto_info()));
    CHECK(automatic.get_mount_info() == "AM7");
    CHECK(held.accepted_connections() == 1);
    CHECK(owner.get_mount_info() == "AM5");
    CHECK(enumerate_zwo_mounts(inputs, 150).empty());
    CHECK(held.accepted_connections() == 1);
    CHECK(free.accepted_connections() == 2);  // one probe, one connected link
}

TEST_CASE("ZWO mount Auto - held-only failure releases only the closing owner's claim",
          "[zwo][network][auto][issue1536]") {
    FakeMountServer server([](const std::string&) { return std::string("AM5#"); });
    REQUIRE(server.ok());
    ZWOMountProtocolWrapper first, second;
    REQUIRE(first.connect(network_info("127.0.0.1", server.port())));
    REQUIRE(second.connect(network_info("127.0.0.1", server.port())));
    REQUIRE(first.get_mount_info() == "AM5");
    REQUIRE(second.get_mount_info() == "AM5");
    ZWOMountProtocolWrapper automatic(false, candidates(server));
    CHECK_FALSE(automatic.connect(auto_info()));
    CHECK_FALSE(automatic.is_connected());
    first.disconnect();
    CHECK_FALSE(automatic.connect(auto_info()));
    CHECK(second.get_mount_info() == "AM5");
    CHECK(server.accepted_connections() == 2);
    second.disconnect();
    REQUIRE(automatic.connect(auto_info()));
    CHECK(automatic.get_mount_info() == "AM5");
    CHECK(server.accepted_connections() == 4);
    ZWOMountProtocolWrapper explicit_sibling;
    CHECK_FALSE(explicit_sibling.connect(network_info("127.0.0.1", server.port())));
    CHECK(server.accepted_connections() == 4);
    automatic.disconnect();
    REQUIRE(explicit_sibling.connect(network_info("127.0.0.1", server.port())));
    CHECK(explicit_sibling.get_mount_info() == "AM5");
}

TEST_CASE("ZWO mount Auto - failed discovery probes release endpoint reservations", "[zwo][network][auto][issue1536]") {
    std::atomic<bool> answer{false};
    FakeMountServer server([&](const std::string&) { return answer.load() ? std::string("AM5#") : std::string(); });
    REQUIRE(server.ok());
    CHECK(enumerate_zwo_mounts(candidates(server), 50).empty());
    answer.store(true);
    {
        ZWOMountProtocolWrapper automatic(false, candidates(server));
        REQUIRE(automatic.connect(auto_info()));
        CHECK(automatic.get_mount_info() == "AM5");
    }
    CHECK(enumerate_zwo_mounts(candidates(server), 150).size() == 1);
}

TEST_CASE("ZWO mount Auto - concurrent attempts cannot share an endpoint",
          "[zwo][network][auto][stress-guard][issue1536]") {
    FakeMountServer server([](const std::string&) { return std::string("AM5#"); });
    REQUIRE(server.ok());
    ZWOMountProtocolWrapper first(false, candidates(server)), second(false, candidates(server));
    std::barrier start(2);
    bool connected[2]{};
    std::jthread a([&] {
        start.arrive_and_wait();
        connected[0] = first.connect(auto_info());
    });
    std::jthread b([&] {
        start.arrive_and_wait();
        connected[1] = second.connect(auto_info());
    });
    a.join();
    b.join();
    CHECK(static_cast<int>(connected[0]) + static_cast<int>(connected[1]) == 1);
    CHECK((connected[0] ? first : second).get_mount_info() == "AM5");
    CHECK(server.accepted_connections() >= 2);
    CHECK(server.accepted_connections() <= 3);  // either one or two successful probes, then one link
}

TEST_CASE("ZWO mount network - refused socket releases its endpoint reservation", "[zwo][network][auto][issue1536]") {
    int port = 0;
    {
        FakeMountServer temporary;
        REQUIRE(temporary.ok());
        port = temporary.port();
    }
    ZWOMountProtocolWrapper refused;
    REQUIRE_FALSE(refused.connect(network_info("127.0.0.1", port)));
    FakeMountServer replacement([](const std::string&) { return std::string("AM5#"); }, INADDR_LOOPBACK,
                                static_cast<std::uint16_t>(port));
    REQUIRE(replacement.ok());
    ZWOMountProtocolWrapper automatic(false, candidates(replacement));
    REQUIRE(automatic.connect(auto_info()));
    CHECK(automatic.get_mount_info() == "AM5");
}

TEST_CASE("ZWO mount Auto - rechecks ownership between discovery and final open", "[zwo][network][auto][issue1536]") {
    FakeMountServer first([](const std::string&) { return std::string("AM5#"); });
    std::promise<void> probing;
    std::promise<void> resume;
    auto released = resume.get_future().share();
    std::atomic<bool> block{true};
    FakeMountServer second([&](const std::string&) {
        if (block.exchange(false)) {
            probing.set_value();
            released.wait();
        }
        return std::string("AM7#");
    });
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    auto inputs = candidates(first);
    inputs.push_back(candidates(second).front());
    ZWOMountProtocolWrapper automatic(false, inputs), owner;
    bool connected = false;
    std::jthread connecting([&] { connected = automatic.connect(auto_info()); });
    const auto status = probing.get_future().wait_for(std::chrono::seconds(5));
    const bool owned = owner.connect(network_info("127.0.0.1", first.port()));
    resume.set_value();
    connecting.join();
    REQUIRE(status == std::future_status::ready);
    REQUIRE(owned);
    REQUIRE(connected);
    CHECK(automatic.get_mount_info() == "AM7");
    CHECK(owner.get_mount_info() == "AM5");
    CHECK(first.accepted_connections() == 2);  // discovery plus the explicit owner, no stale final open
}

TEST_CASE("ZWO mount Auto - deferred close keeps its endpoint reserved", "[zwo][network][auto][issue1536]") {
    std::promise<void> entered;
    std::promise<void> resume;
    auto released = resume.get_future().share();
    std::atomic<bool> block{true};
    FakeMountServer server([&](const std::string&) {
        if (block.exchange(false)) {
            entered.set_value();
            released.wait();
        }
        return std::string("AM5#");
    });
    REQUIRE(server.ok());
    ZWOMountProtocolWrapper owner;
    REQUIRE(owner.connect(network_info("127.0.0.1", server.port())));
    std::exception_ptr error;
    std::jthread reading([&] {
        try {
            (void)owner.get_mount_info();
        } catch (...) {
            error = std::current_exception();
        }
    });
    const auto status = entered.get_future().wait_for(std::chrono::seconds(5));
    owner.try_disconnect();  // the blocked exchange owns the I/O lock
    CHECK(enumerate_zwo_mounts(candidates(server), 50).empty());
    CHECK(server.accepted_connections() == 1);
    resume.set_value();
    reading.join();
    REQUIRE(status == std::future_status::ready);
    CHECK_FALSE(error);
    owner.disconnect();
}

#endif  // !_WIN32

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

#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/zwo/zwo_mount_protocol_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include <barrier>
#include <exception>
#include <future>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"
#include "fake_zwo_mount.h"

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

// The wait diagnostic is emitted outside the registry lock, before its ownership recheck.
struct ProbeWaitLog {
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    alpacacore::logging::LogLevel level = alpacacore::logging::get_log_level();
    std::promise<void> entered;
    std::future<void> waiting = entered.get_future();
    std::atomic<bool> first{true};

    explicit ProbeWaitLog(std::function<void()> on_wait = [] {}) {
        using namespace alpacacore::logging;
        set_log_level(LogLevel::Debug);
        set_log_sink([this, on_wait](LogLevel, std::string_view component, std::string_view message) {
            if (component == "ZWO" && message.starts_with("Waiting up to 1500 ms for a network discovery probe") &&
                first.exchange(false)) {
                entered.set_value();
                on_wait();
            }
        });
    }
    ~ProbeWaitLog() {
        alpacacore::logging::set_log_sink(previous);
        alpacacore::logging::set_log_level(level);
    }
};

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

TEST_CASE("ZWO mount Auto - repeated empty and held-only refusals log at DEBUG", "[zwo][network][auto][issue1619]") {
    using namespace alpacacore::logging;
    std::atomic<int> summaries{0}, warnings{0};
    struct LogGuard {
        LogSink previous = get_log_sink();
        LogLevel level = get_log_level();
        ~LogGuard() {
            set_log_sink(previous);
            set_log_level(level);
        }
    } guard;
    set_log_level(LogLevel::Debug);
    set_log_sink([&](LogLevel level, std::string_view component, std::string_view message) {
        if (component != "ZWO") return;
        if (level >= LogLevel::Warn) ++warnings;
        if (level == LogLevel::Debug && message == "Auto-detect: no ZWO mount found on USB or WiFi") ++summaries;
    });
    FakeMountServer server([](const std::string&) { return std::string("AM5#"); });
    REQUIRE(server.ok());
    ZWOMountProtocolWrapper owner;
    REQUIRE(owner.connect(network_info("127.0.0.1", server.port())));
    REQUIRE(owner.get_mount_info() == "AM5");
    ZWOMountProtocolWrapper empty(false, std::vector<ZWODeviceInfo>{}), held(false, candidates(server));
    for (int attempt = 0; attempt < 3; ++attempt) {
        CHECK_FALSE(empty.connect(auto_info()));
        CHECK_FALSE(held.connect(auto_info()));
        CHECK_FALSE(empty.is_connected());
        CHECK_FALSE(held.is_connected());
    }
    CHECK(summaries.load() == 6);
    CHECK(warnings.load() == 0);
    CHECK(server.accepted_connections() == 1);
    CHECK(owner.get_mount_info() == "AM5");
    owner.disconnect();
    REQUIRE(held.connect(auto_info()));
    CHECK(held.get_mount_info() == "AM5");
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

TEST_CASE("ZWO mount network - explicit connect waits for a discovery probe to release", "[zwo][network][issue1620]") {
    ProbeWaitLog log;
    std::promise<void> probing, resume;
    auto released = resume.get_future().share();
    std::atomic<bool> first{true};
    bool valid = true;
    SECTION("valid discovery") {}
    SECTION("invalid discovery") { valid = false; }
    FakeMountServer server([&](const std::string&) {
        if (first.exchange(false)) {
            probing.set_value();
            released.wait();
            if (!valid) return std::string("NotAMount#");
        }
        return std::string("AM5#");
    });
    REQUIRE(server.ok());
    auto probe = std::async(std::launch::async, [&] { return enumerate_zwo_mounts(candidates(server), 5000); });
    const auto entered = probing.get_future().wait_for(std::chrono::seconds(2));
    ZWOMountProtocolWrapper explicit_link;
    auto connecting =
        std::async(std::launch::async, [&] { return explicit_link.connect(network_info("localhost", server.port())); });
    const auto waited = log.waiting.wait_for(std::chrono::seconds(2));
    const auto pending = connecting.wait_for(std::chrono::milliseconds(0));
    const auto sockets = server.accepted_connections();
    resume.set_value();
    const auto found = probe.get();
    const bool connected = connecting.get();
    REQUIRE(entered == std::future_status::ready);
    CHECK(waited == std::future_status::ready);
    CHECK(pending == std::future_status::timeout);
    CHECK(sockets == 1);
    REQUIRE(found.size() == (valid ? 1 : 0));
    REQUIRE(connected);
    CHECK(explicit_link.get_mount_info() == "AM5");
    CHECK(server.accepted_connections() == 2);
}

TEST_CASE("ZWO mount network - cancelling a probe wait preserves ownership and permits retry",
          "[zwo][network][issue1620]") {
    bool at_release = false;
    SECTION("cancel while probe remains held") {}
    SECTION("cancel before ownership recheck after release") { at_release = true; }
    std::promise<void> recheck;
    auto allowed = recheck.get_future().share();
    ProbeWaitLog log([&] {
        if (at_release) allowed.wait();
    });
    std::promise<void> probing, resume;
    auto released = resume.get_future().share();
    std::atomic<bool> first{true};
    FakeMountServer server([&](const std::string&) {
        if (first.exchange(false)) {
            probing.set_value();
            released.wait();
        }
        return std::string("AM5#");
    });
    REQUIRE(server.ok());
    auto probe = std::async(std::launch::async, [&] { return enumerate_zwo_mounts(candidates(server), 5000); });
    const auto entered = probing.get_future().wait_for(std::chrono::seconds(2));
    ZWOMountProtocolWrapper explicit_link;
    auto connecting =
        std::async(std::launch::async, [&] { return explicit_link.connect(network_info("127.0.0.1", server.port())); });
    const auto waited = log.waiting.wait_for(std::chrono::seconds(2));
    if (at_release) {
        resume.set_value();
        (void)probe.get();
    }
    explicit_link.cancel_pending_io();
    recheck.set_value();
    const auto cancelled = connecting.wait_for(std::chrono::milliseconds(500));
    const auto sockets = server.accepted_connections();
    if (!at_release) {
        resume.set_value();
        (void)probe.get();
    }
    const bool connected = connecting.get();
    REQUIRE(entered == std::future_status::ready);
    REQUIRE(waited == std::future_status::ready);
    CHECK(cancelled == std::future_status::ready);
    CHECK_FALSE(connected);
    CHECK_FALSE(explicit_link.is_connected());
    CHECK(explicit_link.last_connect_error().find("cancelled") != std::string::npos);
    CHECK(sockets == 1);
    REQUIRE(explicit_link.connect(network_info("127.0.0.1", server.port())));
    CHECK(explicit_link.get_mount_info() == "AM5");
    explicit_link.disconnect();
    CHECK(enumerate_zwo_mounts(candidates(server), 500).size() == 1);
}

TEST_CASE("ZWO mount network - probe wait expires despite unrelated endpoint releases", "[zwo][network][issue1620]") {
    using namespace std::chrono_literals;
    ProbeWaitLog log;
    std::promise<void> probing, resume;
    auto released = resume.get_future().share();
    std::atomic<bool> first{true};
    FakeMountServer server([&](const std::string&) {
        if (first.exchange(false)) {
            probing.set_value();
            released.wait();
        }
        return std::string("AM5#");
    });
    FakeMountServer other([](const std::string&) { return std::string("AM7#"); }, INADDR_LOOPBACK + 1);
    REQUIRE(server.ok());
    REQUIRE(other.ok());
    auto probe = std::async(std::launch::async, [&] { return enumerate_zwo_mounts(candidates(server), 5000); });
    const auto entered = probing.get_future().wait_for(2s);
    ZWOMountProtocolWrapper explicit_link;
    const auto start = std::chrono::steady_clock::now();
    auto connecting =
        std::async(std::launch::async, [&] { return explicit_link.connect(network_info("localhost", server.port())); });
    const auto waited = log.waiting.wait_for(2s);
    std::atomic<unsigned> released_others{0};
    std::jthread churn([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            ZWOMountProtocolWrapper link;
            if (link.connect(network_info("127.0.0.2", other.port()))) ++released_others;
            link.disconnect();
            std::this_thread::sleep_for(10ms);  // Real TCP registry notifications, not a driver timer.
        }
    });
    const auto expired = connecting.wait_for(2200ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    churn.request_stop();
    churn.join();
    if (expired != std::future_status::ready) explicit_link.cancel_pending_io();
    const bool connected = connecting.get();
    const auto reason = explicit_link.last_connect_error();
    const auto still_probing = probe.wait_for(0ms);
    const auto sockets = server.accepted_connections();
    // A failed waiter must not erase the still-active probe's claim.
    const auto skipped = enumerate_zwo_mounts(candidates(server), 50);
    resume.set_value();
    (void)probe.get();
    REQUIRE(entered == std::future_status::ready);
    REQUIRE(waited == std::future_status::ready);
    CHECK(expired == std::future_status::ready);
    CHECK(elapsed >= 1400ms);
    CHECK(elapsed < 2200ms);
    CHECK(released_others.load() > 1);
    CHECK_FALSE(connected);
    CHECK(reason.find("1500 ms") != std::string::npos);
    CHECK(still_probing == std::future_status::timeout);
    CHECK(sockets == 1);
    CHECK(skipped.empty());
    REQUIRE(explicit_link.connect(network_info("localhost", server.port())));
    CHECK(explicit_link.get_mount_info() == "AM5");
}

TEST_CASE("ZWO mount network - waiting explicit entry refuses an Auto winner", "[zwo][network][issue1620]") {
    using namespace std::chrono_literals;
    std::promise<void> recheck;
    auto allowed = recheck.get_future().share();
    ProbeWaitLog log([&] { allowed.wait(); });
    std::promise<void> probing, resume;
    auto released = resume.get_future().share();
    std::atomic<bool> first{true};
    FakeMountServer server([&](const std::string&) {
        if (first.exchange(false)) {
            probing.set_value();
            released.wait();
        }
        return std::string("AM5#");
    });
    REQUIRE(server.ok());
    ZWOMountProtocolWrapper automatic(false, candidates(server)), explicit_link;
    auto auto_connect = std::async(std::launch::async, [&] { return automatic.connect(auto_info()); });
    const auto entered = probing.get_future().wait_for(2s);
    auto connecting =
        std::async(std::launch::async, [&] { return explicit_link.connect(network_info("localhost", server.port())); });
    const auto waited = log.waiting.wait_for(2s);
    resume.set_value();
    const bool auto_connected = auto_connect.get();
    recheck.set_value();
    const auto refused = connecting.wait_for(500ms);
    const bool connected = connecting.get();
    REQUIRE(entered == std::future_status::ready);
    REQUIRE(waited == std::future_status::ready);
    REQUIRE(auto_connected);
    CHECK(refused == std::future_status::ready);
    CHECK_FALSE(connected);
    CHECK(automatic.get_mount_info() == "AM5");
    CHECK(server.accepted_connections() == 2);
    // A new explicit caller also refuses immediately, without entering the probe wait.
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(explicit_link.connect(network_info("localhost", server.port())));
    CHECK(std::chrono::steady_clock::now() - start < 500ms);
    CHECK(automatic.get_mount_info() == "AM5");
    CHECK(server.accepted_connections() == 2);
}

TEST_CASE("ZWO mount network - disconnect during probe contention leaves the driver offline",
          "[zwo][network][telescope][issue1620]") {
    using namespace std::chrono_literals;
    bool sync_connect = false, sync_disconnect = false;
    SECTION("async connect and async disconnect") {}
    SECTION("async connect and sync disconnect") { sync_disconnect = true; }
    SECTION("sync connect and sync disconnect") { sync_connect = sync_disconnect = true; }
    ProbeWaitLog log;
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeZwoMount mount(clock, "AM5");
    REQUIRE(mount.ok());
    const std::vector<ZWODeviceInfo> inputs{{ConnectionType::Network, "", "127.0.0.1", mount.port(), "", "loopback"}};
    mount.hold_reply(":GVP");
    auto probe = std::async(std::launch::async, [&] { return enumerate_zwo_mounts(inputs, 5000); });
    const bool probing = mount.wait_until_held(2s);
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, network_info("localhost", mount.port()), clock);
    auto connect = std::async(std::launch::async, [&] {
        if (sync_connect)
            driver->set_connected(true);
        else
            driver->connect();
    });
    const auto waited = log.waiting.wait_for(2s);
    auto getters = std::async(std::launch::async, [&] {
        return !driver->get_connected() && driver->get_connecting() == !sync_connect && !driver->get_name().empty();
    });
    const auto responsive = getters.wait_for(300ms);
    auto disconnect = std::async(std::launch::async, [&] {
        if (sync_disconnect)
            driver->set_connected(false);
        else
            driver->disconnect();
    });
    const auto deferred = disconnect.wait_for(100ms);
    mount.release();
    (void)probe.get();
    connect.get();
    disconnect.get();
    const bool getter_state = getters.get();
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (driver->get_connecting() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();  // Connection-thread completion, not a driver timer.
    }
    const bool finished = !driver->get_connecting();
    const bool offline = !driver->get_connected();
    driver.reset();  // Also joins deferred physical close before testing reservation release.
    REQUIRE(probing);
    REQUIRE(waited == std::future_status::ready);
    CHECK(responsive == std::future_status::ready);
    CHECK(getter_state);
    CHECK(deferred == (sync_connect ? std::future_status::timeout : std::future_status::ready));
    CHECK(finished);
    CHECK(offline);
    CHECK(enumerate_zwo_mounts(inputs, 500).size() == 1);
}

TEST_CASE("ZWO mount network - cancellation during Auto discovery prevents final open", "[zwo][network][issue1620]") {
    std::promise<void> probing, resume;
    auto released = resume.get_future().share();
    std::atomic<bool> first{true};
    FakeMountServer server([&](const std::string&) {
        if (first.exchange(false)) {
            probing.set_value();
            released.wait();
        }
        return std::string("AM5#");
    });
    REQUIRE(server.ok());
    ZWOMountProtocolWrapper automatic(false, candidates(server));
    auto connecting = std::async(std::launch::async, [&] { return automatic.connect(auto_info()); });
    const auto entered = probing.get_future().wait_for(std::chrono::seconds(2));
    automatic.cancel_pending_io();
    resume.set_value();
    const bool connected = connecting.get();
    REQUIRE(entered == std::future_status::ready);
    CHECK_FALSE(connected);
    CHECK_FALSE(automatic.is_connected());
    CHECK(automatic.last_connect_error().find("cancelled") != std::string::npos);
    CHECK(server.accepted_connections() == 1);
    REQUIRE(automatic.connect(auto_info()));
    CHECK(automatic.get_mount_info() == "AM5");
    CHECK(server.accepted_connections() == 3);
}

#endif  // !_WIN32

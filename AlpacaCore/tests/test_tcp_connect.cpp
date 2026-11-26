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

// util::connect_tcp (#2711): the bounded outbound TCP connect, against loopback listeners and injected resolvers.

#ifndef _WIN32

#include <alpacacore/util/tcp_connect.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_mount_server.h"

using alpacacore::test::FakeMountServer;
using alpacacore::test::SilentListener;
using alpacacore::util::connect_tcp;
using alpacacore::util::TcpConnectResult;
using alpacacore::util::TcpLookup;

namespace {

using Clock = std::chrono::steady_clock;

struct Timed {
    TcpConnectResult result;
    Clock::duration took{};
};

template <typename F>
Timed timed(F&& f) {
    const auto start = Clock::now();
    Timed t{f(), {}};
    t.took = Clock::now() - start;
    return t;
}

void close_fd(const TcpConnectResult& r) {
    if (r.fd >= 0) ::close(r.fd);
}

// A resolver that blocks until the case releases it, as a resolver with no answering DNS server does.
struct HungResolver {
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool released = false;
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    ~HungResolver() { release(); }
    void release() {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->released = true;
        state->cv.notify_all();
    }
    alpacacore::util::TcpResolver resolver() const {
        return [s = state](const std::string&, int, int) {
            std::unique_lock<std::mutex> lock(s->mutex);
            s->cv.wait(lock, [&] { return s->released; });
            return TcpLookup{{}, "host not found (released)"};
        };
    }
};

}  // namespace

// Falsified by: tcp_connect.h connect_tcp_address() leaving the socket non-blocking or not close-on-exec
TEST_CASE("TCP connect - a listener that answers gives a blocking, close-on-exec socket", "[util][tcp]") {
    FakeMountServer server;
    REQUIRE(server.ok());
    const auto t = timed([&] { return connect_tcp("127.0.0.1", server.port(), 2000); });
    INFO("error: " << t.result.error);
    REQUIRE(t.result.fd >= 0);
    CHECK(t.result.error.empty());
    CHECK((::fcntl(t.result.fd, F_GETFL) & O_NONBLOCK) == 0);
    CHECK((::fcntl(t.result.fd, F_GETFD) & FD_CLOEXEC) != 0);
    close_fd(t.result);
}

// Falsified by: tcp_connect.h lookup_tcp_host() refusing a host name (only literal addresses resolved)
TEST_CASE("TCP connect - a host name is looked up", "[util][tcp]") {
    FakeMountServer server;
    REQUIRE(server.ok());
    const auto t = timed([&] { return connect_tcp("localhost", server.port(), 2000, nullptr, {}, AF_INET); });
    INFO("error: " << t.result.error);
    CHECK(t.result.fd >= 0);
    close_fd(t.result);
}

// Falsified by: tcp_connect.h connect_tcp_address() waiting the deadline after a refusal, or dropping its errno text
TEST_CASE("TCP connect - a refusal fails at once with the reason", "[util][tcp]") {
    int port = 0;
    {
        FakeMountServer gone;  // its port, now closed
        REQUIRE(gone.ok());
        port = gone.port();
    }
    const auto t = timed([&] { return connect_tcp("127.0.0.1", port, 3000); });
    INFO("error: " << t.result.error);
    CHECK(t.result.fd == -1);
    CHECK(t.took < std::chrono::milliseconds(1000));
    CHECK(t.result.error.find("refused") != std::string::npos);
}

// Falsified by: tcp_connect.h connect_tcp_address() polling without the deadline (waits out the kernel TCP timeout)
TEST_CASE("TCP connect - a host that never answers fails at the deadline", "[util][tcp]") {
    SilentListener silent;
    REQUIRE(silent.ok());  // the accept queue filled: a connect to it gets no answer
    const auto t = timed([&] { return connect_tcp("127.0.0.1", silent.port(), 500); });
    INFO("error: " << t.result.error);
    CHECK(t.result.fd == -1);
    CHECK(t.took >= std::chrono::milliseconds(450));
    CHECK(t.took < std::chrono::milliseconds(1500));
    CHECK(t.result.error == "no answer within 500 ms");
}

// Falsified by: tcp_connect.h connect_tcp_address() not checking the cancel flag between poll slices
TEST_CASE("TCP connect - a cancel ends a pending connect within 50 ms slices", "[util][tcp]") {
    SilentListener silent;
    REQUIRE(silent.ok());
    std::atomic<bool> cancel{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel.store(true);
    });
    const auto t = timed([&] { return connect_tcp("127.0.0.1", silent.port(), 5000, &cancel); });
    canceller.join();
    INFO("error: " << t.result.error);
    CHECK(t.result.fd == -1);
    CHECK(t.took < std::chrono::milliseconds(1000));
    CHECK(t.result.error == "cancelled");
}

// Falsified by: tcp_connect.h lookup_tcp_host() dropping the resolver's reason
TEST_CASE("TCP connect - a host that is not found says so", "[util][tcp]") {
    const auto not_found = [](const std::string&, int, int) {
        return TcpLookup{{}, "host not found (Name or service not known)"};
    };
    const auto t = timed([&] { return connect_tcp("mount.invalid", 4030, 2000, nullptr, not_found); });
    CHECK(t.result.fd == -1);
    CHECK(t.result.error == "host not found (Name or service not known)");
}

// Falsified by: tcp_connect.h lookup_tcp_host() passing on a resolver result with no address and no error (a caller
// that reads addresses.front() on an empty error would read past the end)
TEST_CASE("TCP connect - a lookup with no address and no error is host not found", "[util][tcp]") {
    const auto empty = [](const std::string&, int, int) { return TcpLookup{}; };
    const auto lookup = alpacacore::util::lookup_tcp_host("mount.example", 4030, AF_INET,
                                                          alpacacore::util::TcpDeadline::in(2000), nullptr, empty);
    CHECK(lookup.addresses.empty());
    CHECK(lookup.error == "host not found (no address)");
}

// Falsified by: tcp_connect.h lookup_tcp_host() calling the resolver on the calling thread (a hung lookup holds the
// connect past its deadline)
TEST_CASE("TCP connect - a resolver that hangs fails at the deadline", "[util][tcp]") {
    HungResolver hung;
    const auto t = timed([&] { return connect_tcp("mount.example", 4030, 300, nullptr, hung.resolver()); });
    INFO("error: " << t.result.error);
    CHECK(t.result.fd == -1);
    CHECK(t.took < std::chrono::milliseconds(1000));
    CHECK(t.result.error == "host name lookup did not finish within 300 ms");
}

// Falsified by: tcp_connect.h lookup_tcp_host() not checking the cancel flag while the resolver runs
TEST_CASE("TCP connect - a cancel ends a pending host name lookup", "[util][tcp]") {
    HungResolver hung;
    std::atomic<bool> cancel{true};
    const auto t = timed([&] { return connect_tcp("mount.example", 4030, 5000, &cancel, hung.resolver()); });
    CHECK(t.result.fd == -1);
    CHECK(t.took < std::chrono::milliseconds(500));
    CHECK(t.result.error == "cancelled");
}

#endif  // !_WIN32

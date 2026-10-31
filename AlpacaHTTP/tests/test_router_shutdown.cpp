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

// The delayed thread behind POST shutdown and restart must not read the Router
// after the delay and must not outlive the Server (open-astro#1739).

#include <alpacahttp/request.h>
#include <alpacahttp/router.h>
#include <alpacahttp/server.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "test_assert.h"

namespace {

struct Calls {
    std::mutex mutex;
    std::condition_variable cv;
    int original = 0;
    int replacement = 0;
};

alpacahttp::Response route_post(alpacahttp::Router& router, const std::string& path) {
    alpacahttp::Request request;
    std::ostringstream raw;
    raw << "POST " << path << " HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}";
    EXPECT(request.parse(raw.str()));
    return router.route(request, 1);
}

// POST `path`, then swap the Router's callback before the handler's 100 ms
// delay ends. The callback the request saw must be the one that runs.
void check(const std::string& path, void (alpacahttp::Router::*setter)(std::function<void()>)) {
    Calls calls;
    auto bump = [&calls](int Calls::*counter) {
        return [&calls, counter] {
            std::lock_guard<std::mutex> lock(calls.mutex);
            ++(calls.*counter);
            calls.cv.notify_all();
        };
    };
    auto router = std::make_unique<alpacahttp::Router>();
    (*router.*setter)(bump(&Calls::original));
    EXPECT(route_post(*router, path).status_code() == 200);
    (*router.*setter)(bump(&Calls::replacement));

    std::unique_lock<std::mutex> lock(calls.mutex);
    calls.cv.wait_for(lock, std::chrono::seconds(5), [&] { return calls.original + calls.replacement > 0; });
    EXPECT(calls.original == 1);
    EXPECT(calls.replacement == 0);
}

// POST `path` to a heap Server, destroy the Server at once, and wait past the
// 100 ms delay: the embedder callback must never run, and the delayed thread
// must not reach the dead Server (open-astro#1739; ASan reports it otherwise).
void check_server_teardown(const std::string& path, void (alpacahttp::Server::*setter)(std::function<void()>)) {
    auto count = std::make_shared<std::atomic<int>>(0);
    auto server = std::make_unique<alpacahttp::Server>(alpacahttp::Config{});
    (*server.*setter)([count] { ++*count; });
    EXPECT(route_post(server->router_for_test(), path).status_code() == 200);
    server.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT(*count == 0);
}

// Repeated restarts must not pile up finished threads (open-astro#1739).
// A start reaps finished threads, so keep posting until only the newest is left.
void check_finished_threads_reaped() {
    auto router = std::make_unique<alpacahttp::Router>();
    auto count = std::make_shared<std::atomic<int>>(0);
    router->set_restart_callback([count] { ++*count; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int posts = 0;
    do {
        EXPECT(route_post(*router, "/management/v1/restart").status_code() == 200);
        ++posts;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    } while (posts < 3 || (router->delayed_thread_count_for_test() != 1 && std::chrono::steady_clock::now() < deadline));
    EXPECT(*count >= 3);
    EXPECT(router->delayed_thread_count_for_test() == 1);
}

// A second request during the 100 ms delay is dropped: only the first callback runs.
void check_second_request_dropped() {
    auto router = std::make_unique<alpacahttp::Router>();
    auto restarts = std::make_shared<std::atomic<int>>(0);
    auto shutdowns = std::make_shared<std::atomic<int>>(0);
    router->set_restart_callback([restarts] { ++*restarts; });
    router->set_shutdown_callback([shutdowns] { ++*shutdowns; });
    EXPECT(route_post(*router, "/management/v1/restart").status_code() == 200);
    EXPECT(route_post(*router, "/management/v1/shutdown").status_code() == 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT(*restarts == 1);
    EXPECT(*shutdowns == 0);
}

}  // namespace

int main() {
    std::cout << "Testing router shutdown and restart callbacks...\n";
    // case: POST shutdown runs the callback it saw, not whatever the Router holds later (#1739)
    check("/management/v1/shutdown", &alpacahttp::Router::set_shutdown_callback);
    // case: POST restart runs the callback it saw, not whatever the Router holds later (#1739)
    check("/management/v1/restart", &alpacahttp::Router::set_restart_callback);
    // case: POST shutdown then Server teardown never runs the callback or reaches the Server (#1739)
    check_server_teardown("/management/v1/shutdown", &alpacahttp::Server::set_shutdown_callback);
    // case: POST restart then Server teardown never runs the callback or reaches the Server (#1739)
    check_server_teardown("/management/v1/restart", &alpacahttp::Server::set_restart_callback);
    // case: repeated restarts join their finished delayed threads (#1739)
    check_finished_threads_reaped();
    // case: a second shutdown or restart during the delay is dropped (#1739)
    check_second_request_dropped();
    std::cout << "Router shutdown tests passed\n";
    return 0;
}

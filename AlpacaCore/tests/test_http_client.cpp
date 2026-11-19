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

// util::HttpClient (decision 0023, issue #1753): the libcurl implementation against a loopback HTTP server, and the
// test-only LockedHttpClient decorator. These hold the decision 0007 bounds of an outbound client in one place: the
// response body cap, http and https only on the request and on every redirect, and the per-call timeout.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "fake_http_client.h"
#include "locked_http_client.h"
#include "loopback_http_server.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using alpacacore::test::LoopbackHttpServer;
using alpacacore::util::HttpResponse;
using alpacacore::util::kHttpMaxResponseBytes;

constexpr std::chrono::milliseconds kTimeout{3000};

std::shared_ptr<alpacacore::util::HttpClient> client() {
    return alpacacore::util::make_curl_http_client("AlpacaBridge-test/1.0");
}

// The error a call throws; DriverException is the only one the seam may throw.
template <typename Fn>
std::string driver_error_of(Fn&& fn) {
    try {
        fn();
    } catch (const AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::DriverException);
        return e.what();
    }
    FAIL("the request must throw DriverException");
    return "";
}

}  // namespace

TEST_CASE("HttpClient - GET returns the status and the body", "[util][http-client]") {
    LoopbackHttpServer server;
    server.set_body("{\"ok\":true}");
    const HttpResponse response = client()->get(server.url("/current"), kTimeout);
    CHECK(response.status == 200);
    CHECK(response.body == "{\"ok\":true}");
    CHECK(server.last_request().rfind("GET /current HTTP/1.1\r\n", 0) == 0);
    CHECK(server.last_request().find("User-Agent: AlpacaBridge-test/1.0\r\n") != std::string::npos);
}

TEST_CASE("HttpClient - PUT sends the form body with the form content type", "[util][http-client]") {
    LoopbackHttpServer server;
    server.set_body("{\"ErrorNumber\":0}");
    const std::string form = "Connected=True&ClientID=7&ClientTransactionID=12";
    const HttpResponse response = client()->put_form(server.url("/api/v1/camera/0/connected"), form, kTimeout);
    CHECK(response.status == 200);
    CHECK(response.body == "{\"ErrorNumber\":0}");
    const std::string request = server.last_request();
    CHECK(request.rfind("PUT /api/v1/camera/0/connected HTTP/1.1\r\n", 0) == 0);
    CHECK(request.find("Content-Type: application/x-www-form-urlencoded\r\n") != std::string::npos);
    CHECK(request.find("Content-Length: " + std::to_string(form.size()) + "\r\n") != std::string::npos);
    CHECK(request.substr(request.size() - form.size()) == form);
}

TEST_CASE("HttpClient - an HTTP error status is returned, not thrown", "[util][http-client]") {
    LoopbackHttpServer server;
    server.set_mode(LoopbackHttpServer::Mode::ServerError);
    CHECK(client()->get(server.url(), kTimeout).status == 500);
    CHECK(client()->put_form(server.url(), "a=1", kTimeout).status == 500);
}

TEST_CASE("HttpClient - a body at the 1 MiB cap is read and one past it is refused", "[util][http-client]") {
    for (const bool declare_length : {true, false}) {
        CAPTURE(declare_length);
        LoopbackHttpServer server;
        server.set_declare_length(declare_length);
        server.set_body(std::string(kHttpMaxResponseBytes, 'x'));
        CHECK(client()->get(server.url(), kTimeout).body.size() == kHttpMaxResponseBytes);
        server.set_body(std::string(kHttpMaxResponseBytes + 1, 'x'));
        (void)driver_error_of([&] { (void)client()->get(server.url(), kTimeout); });
        (void)driver_error_of([&] { (void)client()->put_form(server.url(), "a=1", kTimeout); });
    }
}

TEST_CASE("HttpClient - a redirect to http is followed", "[util][http-client]") {
    LoopbackHttpServer target;
    target.set_body("moved here");
    LoopbackHttpServer server;
    server.set_status_line("302 Found");
    server.set_extra_headers("Location: " + target.url("/new") + "\r\n");
    const HttpResponse response = client()->get(server.url(), kTimeout);
    CHECK(response.status == 200);
    CHECK(response.body == "moved here");
    CHECK(target.requests() == 1);
}

TEST_CASE("HttpClient - a redirect to a scheme other than http and https is refused, not followed",
          "[util][http-client]") {
    // The target listens, so a client that followed the ftp redirect would connect to it.
    LoopbackHttpServer target;
    LoopbackHttpServer server;
    server.set_status_line("302 Found");
    server.set_extra_headers("Location: ftp://127.0.0.1:" + std::to_string(target.port()) + "/feed\r\n");
    (void)driver_error_of([&] { (void)client()->get(server.url(), kTimeout); });
    CHECK(server.requests() == 1);
    CHECK(target.requests() == 0);
}

TEST_CASE("HttpClient - a request URL with a scheme other than http and https is refused", "[util][http-client]") {
    LoopbackHttpServer target;
    const std::string text = driver_error_of(
        [&] { (void)client()->get("ftp://127.0.0.1:" + std::to_string(target.port()) + "/", kTimeout); });
    CHECK(text.rfind("HTTP request failed: ", 0) == 0);
    CHECK(target.requests() == 0);
    (void)driver_error_of([&] { (void)client()->get("file:///etc/hostname", kTimeout); });
}

TEST_CASE("HttpClient - a stalled server ends at the call timeout", "[util][http-client]") {
    LoopbackHttpServer server;
    server.set_mode(LoopbackHttpServer::Mode::Stall);
    const auto t0 = std::chrono::steady_clock::now();
    (void)driver_error_of([&] { (void)client()->get(server.url(), std::chrono::milliseconds(300)); });
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    // It waited for the timeout (not a quicker failure) and no longer.
    CHECK(elapsed >= std::chrono::milliseconds(250));
    CHECK(elapsed < std::chrono::seconds(3));
}

TEST_CASE("HttpClient - a PUT is not redirected: its 3xx comes back and the target gets nothing",
          "[util][http-client]") {
    LoopbackHttpServer target;
    LoopbackHttpServer server;
    server.set_status_line("307 Temporary Redirect");
    server.set_extra_headers("Location: " + target.url("/new") + "\r\n");
    const HttpResponse response = client()->put_form(server.url(), "Brightness=5", kTimeout);
    CHECK(response.status == 307);
    CHECK(server.requests() == 1);
    CHECK(target.requests() == 0);
}

TEST_CASE("LockedHttpClient over FakeHttpClient - requests forward one at a time", "[util][http-client]") {
    alpacacore::test::FakeHttpClient fake{[] { return std::string("body"); }};
    fake.set_delay(std::chrono::milliseconds(20));
    alpacacore::test::LockedHttpClient locked(fake.client());
    // The workers only record their answers: Catch2 assertions are not thread-safe, so the CHECKs run after the
    // join.
    std::atomic<int> good_bodies{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&locked, &good_bodies, i] {
            const auto r =
                (i % 2 == 0) ? locked.get("http://x/", kTimeout) : locked.put_form("http://x/", "a=1", kTimeout);
            if (r.body == "body") ++good_bodies;
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    CHECK(good_bodies.load() == 4);
    CHECK(fake.calls() == 4);
    CHECK(fake.max_in_flight() == 1);
    const auto requests = fake.requests();
    REQUIRE(requests.size() == 4);
    int puts = 0;
    for (const auto& r : requests) {
        if (r.method == "PUT") {
            ++puts;
            CHECK(r.form_body == "a=1");
        }
    }
    CHECK(puts == 2);
}

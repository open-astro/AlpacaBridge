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

// The remotealpaca upstream client (decision 0023, issue #1753 slice 2) against the loopback fake Alpaca
// server, over the real libcurl util::HttpClient, and against the scripted FakeHttpClient for the one case
// that counts overlapping calls. Fake evidence only: no network beyond 127.0.0.1.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "fake_alpaca_server.h"
#include "fake_http_client.h"

using alpacacore::AlpacaException;
using alpacacore::test::FakeAlpacaServer;
using alpacacore::vendor::remotealpaca::RemoteAlpacaClient;
using alpacacore::vendor::remotealpaca::RemoteTransportError;
using alpacacore::vendor::remotealpaca::UpstreamDevice;
namespace AlpacaError = alpacacore::AlpacaError;

namespace {

std::shared_ptr<alpacacore::util::HttpClient> curl_client() {
    return alpacacore::util::make_curl_http_client("AlpacaBridge remotealpaca test");
}

UpstreamDevice upstream_at(int port) { return UpstreamDevice{"127.0.0.1", port, "observingconditions", 0}; }

std::string prefix_of(const FakeAlpacaServer& server) { return "127.0.0.1:" + std::to_string(server.port()) + ": "; }

// The exception a call throws, with its type kept: a RemoteTransportError, else an AlpacaException.
template <class Fn>
std::pair<bool, AlpacaException> thrown_by(Fn&& fn) {
    try {
        fn();
    } catch (const RemoteTransportError& e) {
        return {true, e};
    } catch (const AlpacaException& e) {
        return {false, e};
    }
    FAIL("the call did not throw");
    return {false, AlpacaException("unreachable")};
}

}  // namespace

TEST_CASE("Remote Alpaca client - a GET sends canonical casing, its own ClientID and the next ClientTransactionID",
          "[remotealpaca][client]") {
    FakeAlpacaServer server;
    server.set_value("temperature", "21.5");
    server.set_value("skyquality", "19.25");
    RemoteAlpacaClient client(curl_client(), upstream_at(server.port()), 42);

    CHECK(client.get_double("temperature") == 21.5);
    CHECK(client.get_double("temperature") == 21.5);
    CHECK(client.get_double("skyquality", {{"SensorName", "Sky Quality&1"}}) == 19.25);

    const auto requests = server.requests();
    REQUIRE(requests.size() == 3);
    CHECK(requests[0].method == "GET");
    CHECK(requests[0].path == "/api/v1/observingconditions/0/temperature");
    CHECK(requests[0].query == "ClientID=42&ClientTransactionID=1");
    CHECK(requests[1].query == "ClientID=42&ClientTransactionID=2");
    CHECK(requests[2].query == "SensorName=Sky%20Quality%261&ClientID=42&ClientTransactionID=3");
    CHECK(requests[2].body.empty());
}

TEST_CASE("Remote Alpaca client - a PUT sends its parameters in a form body", "[remotealpaca][client]") {
    FakeAlpacaServer server;
    server.set_value("averageperiod", "0");
    RemoteAlpacaClient client(curl_client(), UpstreamDevice{"127.0.0.1", server.port(), "observingconditions", 3}, 7);

    client.put("averageperiod", {{"AveragePeriod", "0.5"}});

    const auto requests = server.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].method == "PUT");
    CHECK(requests[0].path == "/api/v1/observingconditions/3/averageperiod");
    CHECK(requests[0].query.empty());
    CHECK(requests[0].body == "AveragePeriod=0.5&ClientID=7&ClientTransactionID=1");
}

TEST_CASE("Remote Alpaca client - Value is read as its own type", "[remotealpaca][client]") {
    FakeAlpacaServer server;
    server.set_value("connected", "true");
    server.set_value("interfaceversion", "2");
    server.set_value("description", R"("Sim \"OC\" °C\n")");
    server.set_value("supportedactions", R"(["A", "B"])");
    server.set_value("averageperiod", "1.5");
    server.set_value("name", "null");
    RemoteAlpacaClient client(curl_client(), upstream_at(server.port()), 1);

    CHECK(client.get_bool("connected"));
    CHECK(client.get_int("interfaceversion") == 2);
    CHECK(client.get_string("description") ==
          "Sim \"OC\" \xC2\xB0"
          "C\n");
    CHECK(client.get_string_list("supportedactions") == std::vector<std::string>{"A", "B"});
    CHECK(client.get_double("averageperiod") == 1.5);

    // A Value of the wrong type is no usable reply.
    CHECK(thrown_by([&] { client.get_int("averageperiod"); }).first);
    CHECK(thrown_by([&] { client.get_bool("interfaceversion"); }).first);
    CHECK(thrown_by([&] { client.get_string("name"); }).first);
    CHECK(thrown_by([&] { client.get_string_list("description"); }).first);
}

TEST_CASE("Remote Alpaca client - a non-zero ErrorNumber passes through unchanged with the upstream host",
          "[remotealpaca][client]") {
    FakeAlpacaServer server;
    server.set_error("temperature", AlpacaError::InvalidOperation, "Not while parked");
    server.set_error("humidity", 0x501, "Sensor offline");
    RemoteAlpacaClient client(curl_client(), upstream_at(server.port()), 1);

    const auto refused = thrown_by([&] { client.get_double("temperature"); });
    CHECK_FALSE(refused.first);
    CHECK(refused.second.error_code() == AlpacaError::InvalidOperation);
    CHECK(std::string(refused.second.what()) == prefix_of(server) + "Not while parked");

    const auto offline = thrown_by([&] { client.get_double("humidity"); });
    CHECK_FALSE(offline.first);
    CHECK(offline.second.error_code() == 0x501);
    CHECK(std::string(offline.second.what()) == prefix_of(server) + "Sensor offline");

    // An unscripted member is the server's own NotImplemented, passed through.
    const auto missing = thrown_by([&] { client.put("refresh"); });
    CHECK_FALSE(missing.first);
    CHECK(missing.second.error_code() == AlpacaError::NotImplemented);
}

TEST_CASE("Remote Alpaca client - no usable reply is a transport failure", "[remotealpaca][client]") {
    FakeAlpacaServer server;
    server.set_value("temperature", "21.5");
    RemoteAlpacaClient client(curl_client(), upstream_at(server.port()), 1, std::chrono::milliseconds(300));

    for (const auto mode :
         {FakeAlpacaServer::Mode::Stall, FakeAlpacaServer::Mode::Drop, FakeAlpacaServer::Mode::ServerError,
          FakeAlpacaServer::Mode::BadJson, FakeAlpacaServer::Mode::NoErrorNumber}) {
        server.set_mode(mode);
        const auto failure = thrown_by([&] { client.get_double("temperature"); });
        INFO("mode " << static_cast<int>(mode) << ": " << failure.second.what());
        CHECK(failure.first);
        CHECK(failure.second.error_code() == AlpacaError::DriverException);
        CHECK(std::string(failure.second.what()).rfind(prefix_of(server), 0) == 0);
    }

    // A good reply with no Value is no usable reply either.
    server.set_mode(FakeAlpacaServer::Mode::Good);
    server.set_value("temperature", "");
    CHECK(thrown_by([&] { client.get_double("temperature"); }).first);

    // Nothing listens on 127.0.0.1:1: the connection is refused.
    RemoteAlpacaClient nowhere(curl_client(), UpstreamDevice{"127.0.0.1", 1, "observingconditions", 0}, 1,
                               std::chrono::milliseconds(300));
    CHECK(thrown_by([&] { nowhere.get_double("temperature"); }).first);
}

TEST_CASE("Remote Alpaca client - a refused connection is RemoteConnectRefused, other failures are not",
          "[remotealpaca][client]") {
    alpacacore::test::FakeHttpClient http(
        [] { return std::string(R"({"Value":1.0,"ClientTransactionID":0,"ErrorNumber":0,"ErrorMessage":""})"); });
    RemoteAlpacaClient client(http.client(), upstream_at(11111), 9);
    const auto refused_by = [&client](auto&& call) {
        try {
            call();
        } catch (const alpacacore::vendor::remotealpaca::RemoteConnectRefused& e) {
            CHECK(e.error_code() == AlpacaError::DriverException);
            CHECK(std::string(e.what()).rfind("127.0.0.1:11111: ", 0) == 0);
            return true;
        } catch (const RemoteTransportError&) {
            return false;
        }
        FAIL("the call did not throw");
        return false;
    };

    http.refuse_next(2);
    CHECK(refused_by([&] { client.get_double("temperature"); }));
    CHECK(refused_by([&] { client.put("refresh"); }));
    http.fail_next(1);  // a timeout or a dropped connection
    CHECK_FALSE(refused_by([&] { client.get_double("temperature"); }));
    CHECK(client.get_double("temperature") == 1.0);
}

TEST_CASE("Remote Alpaca client - calls on one client never overlap", "[remotealpaca][client]") {
    alpacacore::test::FakeHttpClient http(
        [] { return std::string(R"({"Value":1.0,"ClientTransactionID":0,"ErrorNumber":0,"ErrorMessage":""})"); });
    http.set_delay(std::chrono::milliseconds(20));
    RemoteAlpacaClient client(http.client(), upstream_at(11111), 9);

    std::vector<std::thread> callers;
    for (int t = 0; t < 4; ++t) {
        callers.emplace_back([&client] {
            for (int i = 0; i < 3; ++i) client.get_double("temperature");
        });
    }
    for (auto& caller : callers) caller.join();

    CHECK(http.max_in_flight() == 1);
    std::set<std::string> ids;
    for (const auto& request : http.requests()) {
        ids.insert(request.url.substr(request.url.find("ClientTransactionID=")));
        CHECK(request.timeout == alpacacore::vendor::remotealpaca::kRemoteCallTimeout);
    }
    CHECK(ids.size() == 12);
}

TEST_CASE("Remote Alpaca client - a malformed upstream is refused when the client is built", "[remotealpaca][client]") {
    alpacacore::test::FakeHttpClient http;
    const auto refused = [&](UpstreamDevice upstream) {
        try {
            RemoteAlpacaClient client(http.client(), std::move(upstream), 1);
        } catch (const AlpacaException& e) {
            return e.error_code() == AlpacaError::InvalidValue;
        }
        return false;
    };
    CHECK(refused({"", 11111, "observingconditions", 0}));
    CHECK(refused({"bad host", 11111, "observingconditions", 0}));
    CHECK(refused({"http://host", 11111, "observingconditions", 0}));
    CHECK(refused({"host/path", 11111, "observingconditions", 0}));
    CHECK(refused({"10.0.0.5:11111", 11111, "observingconditions", 0}));  // a port in the host is not IPv6
    CHECK(refused({"host", 0, "observingconditions", 0}));
    CHECK(refused({"host", 65536, "observingconditions", 0}));
    CHECK(refused({"host", 11111, "ObservingConditions", 0}));
    CHECK(refused({"host", 11111, "observingconditions", -1}));

    RemoteAlpacaClient ipv6(http.client(), {"::1", 32323, "safetymonitor", 2}, 1);
    CHECK(ipv6.member_url("issafe") == "http://[::1]:32323/api/v1/safetymonitor/2/issafe");
    RemoteAlpacaClient named(http.client(), {"omnisim.local", 32323, "safetymonitor", 0}, 1);
    CHECK(named.member_url("issafe") == "http://omnisim.local:32323/api/v1/safetymonitor/0/issafe");
}

TEST_CASE("Remote Alpaca client - a malformed member name is refused before any request", "[remotealpaca][client]") {
    FakeAlpacaServer server;
    server.set_value("temperature", "21.5");
    RemoteAlpacaClient client(curl_client(), upstream_at(server.port()), 1);
    const auto invalid = [](auto&& fn) {
        const auto [transport, error] = thrown_by(fn);
        return !transport && error.error_code() == AlpacaError::InvalidValue;
    };

    CHECK(invalid([&] { (void)client.get_double("Temperature"); }));
    CHECK(invalid([&] { (void)client.get_bool("is_safe"); }));
    CHECK(invalid([&] { (void)client.get_string("temperature/../x"); }));
    CHECK(invalid([&] { client.put(""); }));
    CHECK(server.requests().empty());

    CHECK(client.get_double("temperature") == 21.5);  // the next good call is still sent
    CHECK(server.requests().size() == 1);
}

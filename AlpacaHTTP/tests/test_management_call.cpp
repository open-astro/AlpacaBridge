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

// ManagementCall (the ClientTransactionID, body lookup, envelope and exception
// map of the management handlers, issue #2486) on a parsed Request: no Router.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>

#include "../src/http/management_call.h"
#include "test_assert.h"

namespace {

using alpacahttp::ManagementCall;
using alpacahttp::Response;

constexpr std::uint32_t kServerTx = 88;

std::unique_ptr<alpacahttp::Request> make(const std::string& verb, const std::string& target,
                                          const std::string& body = std::string(),
                                          const std::string& content_type = "application/json") {
    auto request = std::make_unique<alpacahttp::Request>();
    std::ostringstream raw;
    raw << verb << " " << target << " HTTP/1.1\r\nHost: localhost\r\n";
    if (!body.empty()) {
        raw << "Content-Type: " << content_type << "\r\nContent-Length: " << body.size() << "\r\n";
    }
    raw << "\r\n" << body;
    EXPECT(request->parse(raw.str()));
    return request;
}

std::uint32_t tx_of(const std::string& verb, const std::string& target, const std::string& body = std::string()) {
    auto request = make(verb, target, body);
    return ManagementCall(*request, kServerTx).client_tx_id();
}

// The tx id after the handler has read the JSON body.
std::uint32_t tx_after_body(const std::string& target, const std::string& body,
                            const std::string& content_type = "application/json") {
    auto request = make("PUT", target, body, content_type);
    const ManagementCall call(*request, kServerTx);
    (void)call.json_body();
    return call.client_tx_id();
}

nlohmann::json envelope(const Response& response) {
    const auto json = nlohmann::json::parse(response.body(), nullptr, false);
    EXPECT(json.is_object());
    return json;
}

}  // namespace

int main() {
    {
        // case: management ClientTransactionID comes from the query in any case, else 0
        EXPECT(tx_of("GET", "/management/v1/logs?ClientTransactionID=7") == 7);
        EXPECT(tx_of("GET", "/management/v1/logs?clienttransactionid=8") == 8);
        EXPECT(tx_of("DELETE", "/management/v1/logfiles?CLIENTTRANSACTIONID=9") == 9);
        EXPECT(tx_of("GET", "/management/v1/logs") == 0);
        EXPECT(tx_of("GET", "/management/v1/logs?ClientTransactionID=abc") == 0);
        EXPECT(tx_of("GET", "/management/v1/logs?ClientTransactionID=-1") == 0);
        EXPECT(tx_of("GET", "/management/v1/logs?ClientTransactionID=4294967296") == 0);
        EXPECT(tx_of("GET", "/management/v1/logs?ClientTransactionID=4294967295") == 4294967295u);
        // Until the handler reads the body, a body ID is not seen.
        EXPECT(tx_of("PUT", "/management/v1/loglevel?ClientTransactionID=1", R"({"ClientTransactionID":11})") == 1);
    }
    {
        // case: a non-zero JSON body ClientTransactionID replaces the query one once the body is read
        EXPECT(tx_after_body("/x?ClientTransactionID=1", R"({"ClientTransactionID":11})") == 11);
        EXPECT(tx_after_body("/x", R"({"clienttransactionid":12})") == 12);
        // A zero, negative, too large or non-integer body ID keeps the query one.
        EXPECT(tx_after_body("/x?ClientTransactionID=2", R"({"ClientTransactionID":0})") == 2);
        EXPECT(tx_after_body("/x?ClientTransactionID=3", R"({"ClientTransactionID":-5})") == 3);
        EXPECT(tx_after_body("/x?ClientTransactionID=4", R"({"ClientTransactionID":4294967296})") == 4);
        EXPECT(tx_after_body("/x?ClientTransactionID=5", R"({"ClientTransactionID":"13"})") == 5);
        // A body that is not JSON (a form body included) is never a source.
        EXPECT(tx_after_body("/x?ClientTransactionID=6", "{not json") == 6);
        EXPECT(tx_after_body("/x?ClientTransactionID=7", "ClientTransactionID=22&Level=INFO",
                             "application/x-www-form-urlencoded") == 7);
        EXPECT(tx_after_body("/x", "ClientTransactionID=22", "application/x-www-form-urlencoded") == 0);
    }
    {
        // case: json_field returns the first spelling the JSON body has
        auto request = make("PUT", "/x", R"({"Level":"INFO","level":"DEBUG","Other":1})");
        const ManagementCall call(*request, kServerTx);
        EXPECT(call.json_body() != nullptr);
        EXPECT(call.json_body() == call.json_body());
        const auto* level = call.json_field({"level", "Level"});
        EXPECT(level != nullptr && *level == "DEBUG");
        const auto* upper = call.json_field({"Level", "level"});
        EXPECT(upper != nullptr && *upper == "INFO");
        // Spellings match exactly: "LEVEL" is not "Level".
        EXPECT(call.json_field({"LEVEL"}) == nullptr);
        EXPECT(call.json_field({"Missing", "Other"}) != nullptr);

        auto empty = make("PUT", "/x");
        EXPECT(ManagementCall(*empty, kServerTx).json_body() == nullptr);
        EXPECT(ManagementCall(*empty, kServerTx).json_field({"level"}) == nullptr);
        auto not_json = make("PUT", "/x", "level=INFO", "application/x-www-form-urlencoded");
        EXPECT(ManagementCall(*not_json, kServerTx).json_field({"level"}) == nullptr);
        auto array = make("PUT", "/x", R"(["level"])");
        EXPECT(ManagementCall(*array, kServerTx).json_field({"level"}) == nullptr);
    }
    {
        // case: management ok and error carry both transaction ids in the JSON envelope
        auto request = make("GET", "/x?ClientTransactionID=9");
        const ManagementCall call(*request, kServerTx);
        const Response ok = call.ok(nlohmann::json{{"Deleted", "a.log"}});
        EXPECT(ok.status_code() == 200);
        EXPECT(ok.get_header("Content-Type") == "application/json");
        const auto ok_json = envelope(ok);
        EXPECT(ok_json["ClientTransactionID"] == 9);
        EXPECT(ok_json["ServerTransactionID"] == kServerTx);
        EXPECT(ok_json["ErrorNumber"] == 0);
        EXPECT(ok_json["ErrorMessage"] == "");
        EXPECT(ok_json["Value"] == nlohmann::json({{"Deleted", "a.log"}}));
        // A string Value stays a string.
        EXPECT(envelope(call.ok(std::string("12345")))["Value"] == "12345");

        const Response error = call.error(0x40B, "busy");
        EXPECT(error.status_code() == 200);
        EXPECT(error.get_header("Content-Type") == "application/json");
        const auto error_json = envelope(error);
        EXPECT(error_json["ClientTransactionID"] == 9);
        EXPECT(error_json["ServerTransactionID"] == kServerTx);
        EXPECT(error_json["ErrorNumber"] == 0x40B);
        EXPECT(error_json["ErrorMessage"] == "busy");
        EXPECT(!error_json.contains("Value"));
    }
    {
        // case: management failure maps an AlpacaException to its number and anything else to DriverError
        auto request = make("GET", "/x?ClientTransactionID=4");
        const ManagementCall call(*request, kServerTx);
        const auto invalid = envelope(call.failure(
            "Failed to x: ", alpacacore::AlpacaException("bad name", alpacacore::AlpacaError::InvalidValue)));
        EXPECT(invalid["ErrorNumber"] == 0x401);
        EXPECT(invalid["ErrorMessage"] == "Failed to x: bad name");
        EXPECT(invalid["ClientTransactionID"] == 4);
        const auto not_connected =
            envelope(call.failure("", alpacacore::AlpacaException("gone", alpacacore::AlpacaError::NotConnected)));
        EXPECT(not_connected["ErrorNumber"] == 0x407);
        EXPECT(not_connected["ErrorMessage"] == "gone");
        const auto other =
            envelope(call.failure("Failed to read log file: ", std::runtime_error("Log file not found")));
        EXPECT(other["ErrorNumber"] == 0x500);
        EXPECT(other["ErrorMessage"] == "Failed to read log file: Log file not found");
        EXPECT(other["ServerTransactionID"] == kServerTx);
    }
    std::cout << "test_management_call: all checks passed\n";
    return 0;
}

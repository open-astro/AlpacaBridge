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

// DeviceCall (the per-request parameter, envelope and error-policy module of
// the device dispatchers) on a parsed Request: no Router, no driver.
//
// Every case carries a "Falsified by:" line naming the mutation of
// device_call.cpp that turns it red.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>
#include <alpacahttp/util/error_mapping.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>

#include "../src/http/device_call.h"
#include "test_assert.h"

namespace {

using alpacahttp::DeviceCall;
using alpacahttp::Response;

constexpr std::uint32_t kServerTx = 77;
constexpr int kInvalidValue = 0x401;
constexpr int kDriverError = 0x500;

// `target` is the request target with its query string. A non-empty body is
// sent with the given content type.
std::unique_ptr<alpacahttp::Request> make(const std::string& verb, const std::string& target,
                                          const std::string& body = std::string(),
                                          const std::string& content_type = "application/x-www-form-urlencoded") {
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

std::unique_ptr<alpacahttp::Request> put_json(const std::string& body) {
    return make("PUT", "/api/v1/telescope/0/x", body, "application/json");
}

std::unique_ptr<alpacahttp::Request> put_form(const std::string& body) {
    return make("PUT", "/api/v1/telescope/0/x", body);
}

nlohmann::json envelope(const Response& response) {
    const auto json = nlohmann::json::parse(response.body(), nullptr, false);
    EXPECT(json.is_object());
    return json;
}

template <typename Fn>
bool throws_bad_request(Fn&& fn, const std::string& message) {
    try {
        fn();
    } catch (const alpacahttp::RequestNotInterpretable& e) {
        return message == e.what();
    }
    return false;
}

// Falsified by: reordering find_raw to try the JSON body before the query string.
void case_query_before_json() {
    auto request = make("PUT", "/x?Tracking=true", "{\"Tracking\":false}", "application/json");
    DeviceCall call(*request, kServerTx);
    EXPECT(call.get_bool("Tracking") == true);
}

// Falsified by: reordering find_raw to try the JSON "Value" before the JSON key named after the parameter.
void case_json_key_before_value() {
    auto request = put_json("{\"Value\":1.5,\"Rate\":2.5}");
    DeviceCall call(*request, kServerTx);
    EXPECT(call.get_double("Rate") == 2.5);
}

// Falsified by: deleting the JSON "Value" fallback from find_raw (the telescope's double and int lacked it).
void case_json_value_fallback() {
    auto request = put_json("{\"Value\":2}");
    DeviceCall call(*request, kServerTx);
    EXPECT(call.get_double("Rate") == 2.0);
    EXPECT(call.get_int("Position") == 2);
    auto text = put_json("{\"Value\":\"abc\"}");
    DeviceCall text_call(*text, kServerTx);
    EXPECT(text_call.get_string("Name") == "abc");
}

// Falsified by: trying the form body before the JSON body in find_raw (the body is JSON and holds a form pair).
void case_json_before_form() {
    auto request = put_json("{\"Position\":3,\"x\":\"&Position=4&\"}");
    DeviceCall call(*request, kServerTx);
    EXPECT(call.get_int("Position") == 3);
    EXPECT(call.form_field("Position") == std::optional<std::string>("4"));
    auto value = put_json("{\"Value\":3,\"x\":\"&Position=4&\"}");
    DeviceCall value_call(*value, kServerTx);
    EXPECT(value_call.get_int("Position") == 3);
}

// Falsified by: comparing form keys without to_lower_copy in get_form_value; dropping url_decode of the value.
void case_form_case_insensitive_and_decoded() {
    auto request = put_form("sitelatitude=-33.856784&NAME=a%20b+c&Value=9");
    DeviceCall call(*request, kServerTx);
    EXPECT(call.get_double("SiteLatitude") == -33.856784);
    EXPECT(call.get_string("Name") == "a b c");
    // The form "Value" is the last resort.
    EXPECT(call.get_int("Other") == 9);
}

// Falsified by: deleting the form "Value" fallback from find_raw.
void case_form_value_fallback() {
    auto request = put_form("Value=true");
    DeviceCall call(*request, kServerTx);
    EXPECT(call.get_bool("Tracking") == true);
}

// Falsified by: find_*() returning a default instead of nullopt, or get_*() not throwing on nullopt.
void case_missing_parameter() {
    auto request = put_form("Other=1");
    DeviceCall call(*request, kServerTx);
    EXPECT(throws_bad_request([&] { call.get_double("Rate"); }, "Missing parameter: Rate"));
    EXPECT(throws_bad_request([&] { call.get_int("Rate"); }, "Missing parameter: Rate"));
    EXPECT(throws_bad_request([&] { call.get_bool("Rate"); }, "Missing parameter: Rate"));
    EXPECT(throws_bad_request([&] { call.get_string("Rate"); }, "Missing parameter: Rate"));
    EXPECT(!call.find_double("Rate").has_value());
    EXPECT(!call.find_int("Rate").has_value());
    EXPECT(!call.find_bool("Rate").has_value());
    EXPECT(!call.find_string("Rate").has_value());
    auto bare = make("GET", "/x");
    DeviceCall bare_call(*bare, kServerTx);
    EXPECT(throws_bad_request([&] { bare_call.get_int("Position"); }, "Missing parameter: Position"));
}

// Falsified by: letting parse_double_value accept "nan" or hex floats; changing the 400 texts.
void case_unparsable_parameter() {
    auto query = make("GET", "/x?Rate=nan&Position=0.5&Tracking=maybe");
    DeviceCall call(*query, kServerTx);
    EXPECT(throws_bad_request([&] { call.get_double("Rate"); }, "Invalid value for parameter: Rate"));
    EXPECT(throws_bad_request([&] { call.get_int("Position"); }, "Invalid value for parameter: Position"));
    EXPECT(throws_bad_request([&] { call.get_bool("Tracking"); }, "Invalid value for parameter: Tracking"));
    // A JSON value of the wrong type, and a fractional or out-of-int JSON integer.
    auto json = put_json("{\"Rate\":true,\"Position\":0.5,\"Big\":4294967296,\"Tracking\":3,\"Name\":4}");
    DeviceCall json_call(*json, kServerTx);
    EXPECT(throws_bad_request([&] { json_call.get_double("Rate"); }, "Invalid JSON value for parameter: Rate"));
    EXPECT(throws_bad_request([&] { json_call.get_int("Position"); }, "Invalid value for parameter: Position"));
    EXPECT(throws_bad_request([&] { json_call.get_int("Big"); }, "Invalid value for parameter: Big"));
    EXPECT(throws_bad_request([&] { json_call.get_bool("Tracking"); }, "Invalid JSON value for parameter: Tracking"));
    EXPECT(throws_bad_request([&] { json_call.get_string("Name"); }, "Invalid JSON value for parameter: Name"));
    // find_*() still throws for a present but unparsable value.
    EXPECT(throws_bad_request([&] { call.find_double("Rate"); }, "Invalid value for parameter: Rate"));
}

// run() answers a missing or unparsable parameter with HTTP 400 text/plain, the message as the body.
// Falsified by: answering RequestNotInterpretable with the 200 envelope in run().
void case_run_bad_request_is_400_text() {
    auto request = put_form("");
    DeviceCall call(*request, kServerTx);
    const auto response = call.run("test method", [&]() -> Response {
        call.get_int("Position");
        return call.ok_void();
    });
    EXPECT(response.status_code() == 400);
    EXPECT(response.get_header("Content-Type") == "text/plain");
    EXPECT(response.body() == "Missing parameter: Position");

    auto bad = make("GET", "/x?Position=abc");
    DeviceCall bad_call(*bad, kServerTx);
    const auto bad_response = bad_call.run("test method", [&]() -> Response {
        bad_call.get_int("Position");
        return bad_call.ok_void();
    });
    EXPECT(bad_response.status_code() == 400);
    EXPECT(bad_response.body() == "Invalid value for parameter: Position");
}

// A value that parses but the body refuses stays HTTP 200 with 0x401, never 400.
// Falsified by: catching AlpacaException(InvalidValue) as a bad request in run().
void case_run_invalid_value_stays_200() {
    auto request = make("PUT", "/x?ClientTransactionID=5", "Position=99");
    DeviceCall call(*request, kServerTx);
    const auto response = call.run("test method", [&]() -> Response {
        const int position = call.get_int("Position");
        throw alpacacore::AlpacaException("Position " + std::to_string(position) + " out of range",
                                          alpacacore::AlpacaError::InvalidValue);
    });
    EXPECT(response.status_code() == 200);
    const auto json = envelope(response);
    EXPECT(json["ErrorNumber"] == kInvalidValue);
    EXPECT(json["ErrorMessage"] == "Position 99 out of range");
}

// Falsified by: returning the raw AlpacaException code instead of map_error_code, or omitting a transaction id.
void case_run_alpaca_exception_mapped_with_both_ids() {
    auto request = make("GET", "/x?ClientTransactionID=42");
    DeviceCall call(*request, kServerTx);
    const auto response = call.run("test method", [&]() -> Response {
        throw alpacacore::AlpacaException("not connected", alpacacore::AlpacaError::NotConnected);
    });
    EXPECT(response.status_code() == 200);
    const auto json = envelope(response);
    EXPECT(json["ErrorNumber"] == 0x407);
    EXPECT(json["ErrorMessage"] == "not connected");
    EXPECT(json["ClientTransactionID"] == 42);
    EXPECT(json["ServerTransactionID"] == kServerTx);
    // An unmapped number collapses to the driver error.
    const auto other = call.run("test method", [&]() -> Response { throw alpacacore::AlpacaException("odd", 0x4ff); });
    EXPECT(envelope(other)["ErrorNumber"] == kDriverError);
}

// A foreign exception is DriverError with its own text; the message is not guessed into another code.
// Falsified by: reinstating the substring guess (an invalid_argument whose text says "invalid value" mapping to 0x401).
void case_run_std_exception_is_driver_error() {
    auto request = make("GET", "/x");
    DeviceCall call(*request, kServerTx);
    const auto response =
        call.run("test method", [&]() -> Response { throw std::invalid_argument("invalid value: not connected"); });
    EXPECT(response.status_code() == 200);
    const auto json = envelope(response);
    EXPECT(json["ErrorNumber"] == kDriverError);
    EXPECT(json["ErrorMessage"] == "invalid value: not connected");
    EXPECT(json["ServerTransactionID"] == kServerTx);
}

// The outer handle_device catch and the management handlers map foreign exceptions with
// exception_to_error_code; its text-based guess is gone.
// Falsified by: reinstating the substring tail in exception_to_error_code (this text mapping to 0x401 or 0x407).
void case_exception_to_error_code_ignores_text() {
    EXPECT(alpacahttp::util::exception_to_error_code(std::invalid_argument("invalid value: not connected")) ==
           kDriverError);
    EXPECT(alpacahttp::util::exception_to_error_code(
               alpacacore::AlpacaException("x", alpacacore::AlpacaError::NotConnected)) == 0x407);
}

// ok() and ok_void() build the success envelope with both ids and no error.
// Falsified by: swapping the two ids in ok(); giving ok_void() a Value.
void case_ok_envelope() {
    auto request = make("GET", "/x?ClientTransactionID=9");
    DeviceCall call(*request, kServerTx);
    const auto number = envelope(call.ok(12.5));
    EXPECT(number["Value"] == 12.5);
    EXPECT(number["ErrorNumber"] == 0);
    EXPECT(number["ClientTransactionID"] == 9);
    EXPECT(number["ServerTransactionID"] == kServerTx);
    EXPECT(envelope(call.ok(std::string("abc")))["Value"] == "abc");
    const auto none = envelope(call.ok_void());
    EXPECT(!none.contains("Value"));
    EXPECT(none["ClientTransactionID"] == 9);
}

// Falsified by: parsing the body per read (body_parse_count grows with each get_*) or re-parsing on a failed parse.
void case_body_parsed_once() {
    auto good = put_json("{\"A\":1,\"B\":2.5,\"C\":true,\"D\":\"x\"}");
    DeviceCall good_call(*good, kServerTx);
    EXPECT(good_call.get_int("A") == 1);
    EXPECT(good_call.get_double("B") == 2.5);
    EXPECT(good_call.get_bool("C") == true);
    EXPECT(good_call.get_string("D") == "x");
    EXPECT(good_call.json_field("A") != nullptr);
    EXPECT(good_call.body_parse_count() == 1);

    // A malformed JSON body fails exactly once, however many parameters are read.
    auto bad = put_form("A=1&B=2&C=true");
    DeviceCall bad_call(*bad, kServerTx);
    EXPECT(bad_call.get_int("A") == 1);
    EXPECT(bad_call.get_int("B") == 2);
    EXPECT(bad_call.get_bool("C") == true);
    EXPECT(!bad_call.find_int("Missing").has_value());
    EXPECT(bad_call.body_parse_count() == 1);

    // GET without a body never parses.
    auto get = make("GET", "/x?A=1");
    DeviceCall get_call(*get, kServerTx);
    EXPECT(get_call.get_int("A") == 1);
    EXPECT(get_call.body_parse_count() == 0);
}

// ClientTransactionID with today's precedence.
// Falsified by: reading the query on PUT, the body on GET, or dropping the form branch.
void case_client_transaction_id() {
    auto put_json_request = make("PUT", "/x?ClientTransactionID=1", "{\"ClientTransactionID\":11}", "application/json");
    EXPECT(DeviceCall(*put_json_request, kServerTx).client_tx_id() == 11);
    auto put_form_request = make("PUT", "/x?ClientTransactionID=1", "clienttransactionid=22&A=1");
    EXPECT(DeviceCall(*put_form_request, kServerTx).client_tx_id() == 22);
    auto get_request = make("GET", "/x?ClientTransactionID=33", "");
    EXPECT(DeviceCall(*get_request, kServerTx).client_tx_id() == 33);
    auto put_empty = make("PUT", "/x?ClientTransactionID=44");
    EXPECT(DeviceCall(*put_empty, kServerTx).client_tx_id() == 0);
    auto get_none = make("GET", "/x");
    EXPECT(DeviceCall(*get_none, kServerTx).client_tx_id() == 0);
    auto negative = make("GET", "/x?ClientTransactionID=-1");
    EXPECT(DeviceCall(*negative, kServerTx).client_tx_id() == 0);
    EXPECT(DeviceCall(*get_request, kServerTx).server_tx_id() == kServerTx);
}

}  // namespace

int main() {
    case_query_before_json();
    case_json_key_before_value();
    case_json_value_fallback();
    case_json_before_form();
    case_form_case_insensitive_and_decoded();
    case_form_value_fallback();
    case_missing_parameter();
    case_unparsable_parameter();
    case_run_bad_request_is_400_text();
    case_run_invalid_value_stays_200();
    case_run_alpaca_exception_mapped_with_both_ids();
    case_run_std_exception_is_driver_error();
    case_exception_to_error_code_ignores_text();
    case_ok_envelope();
    case_body_parsed_once();
    case_client_transaction_id();
    std::cout << "test_device_call: all checks passed\n";
    return 0;
}

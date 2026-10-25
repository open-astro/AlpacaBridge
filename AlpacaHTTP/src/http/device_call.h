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

#pragma once

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacahttp/json_utils.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace alpacahttp {

// A Device API request the server cannot interpret: a required parameter is
// missing, or its value does not parse as the required type. DeviceCall::run()
// answers HTTP 400 text/plain (Alpaca API Reference s2.5, decision 0007 rule 13)
// and never reaches the driver. It is an AlpacaException with InvalidValue so a
// caller that only wants the old error still sees one. A value that parses but
// is out of range is not this: the driver refuses it with an AlpacaException,
// which stays HTTP 200 + an Alpaca error number.
class RequestNotInterpretable : public alpacacore::AlpacaException {
public:
    using alpacacore::AlpacaException::AlpacaException;
};

[[noreturn]] void throw_bad_request(const std::string& message);

// Request-text helpers shared with the management handlers in router.cpp.
std::string to_lower_copy(std::string value);
std::uint32_t parse_client_transaction_id(const std::string& value);
double parse_double_value(const std::string& raw, const std::string& param_name);
int json_int_value(const nlohmann::json& value, const std::string& param_name);
bool parse_bool_value(const std::string& raw, const std::string& param_name);
const nlohmann::json* find_json_value(const nlohmann::json& json_obj, const std::string& key);
std::optional<std::string> get_form_value(std::string_view body, const std::string& key);

// Logs an AlpacaException at the level its error number deserves (an expected
// client-visible refusal is debug, anything else error).
void log_alpaca_exception(const std::string& context, const alpacacore::AlpacaException& e);

// One Device API call: the request plus the server transaction id, built once
// per request by Router::handle_device and handed to the device-type
// dispatchers. It owns the three things every dispatcher used to repeat:
//  - parameters: get_*()/find_*() read one parameter by name, query first, then
//    the JSON body (the name, then "Value"), then the form body (the name, then
//    "Value"); the body is parsed at most once;
//  - the envelope: ok()/ok_void() build the success response with both
//    transaction ids;
//  - the error policy: run() is the one place that decides a missing or
//    unparsable parameter is HTTP 400 text and a refused or failed operation is
//    HTTP 200 with an Alpaca error number.
class DeviceCall {
public:
    DeviceCall(const Request& request, std::uint32_t server_tx_id);

    const Request& request() const { return request_; }
    std::uint32_t client_tx_id() const { return client_tx_id_; }
    std::uint32_t server_tx_id() const { return server_tx_id_; }

    // Required parameters: throw RequestNotInterpretable when missing or unparsable.
    double get_double(const std::string& name) const;
    int get_int(const std::string& name) const;
    bool get_bool(const std::string& name) const;
    std::string get_string(const std::string& name) const;

    // Optional parameters: nullopt when missing; an unparsable value still throws.
    std::optional<double> find_double(const std::string& name) const;
    std::optional<int> find_int(const std::string& name) const;
    std::optional<bool> find_bool(const std::string& name) const;
    std::optional<std::string> find_string(const std::string& name) const;

    // Raw body access for the few arms whose parameters are not scalars (an
    // array, a JSON object dumped as text): the JSON member or form field
    // named `name`, nullptr/nullopt when absent. Uses the one body parse.
    const nlohmann::json* json_field(const std::string& name) const;
    std::optional<std::string> form_field(const std::string& name) const;

    // How many times the body was handed to the JSON parser (0 or 1).
    std::size_t body_parse_count() const { return body_parse_count_; }

    template <typename T>
    Response ok(const T& value) const {
        Response response;
        response.set_body(make_success_response(client_tx_id_, server_tx_id_, value));
        return response;
    }
    Response ok_void() const;
    // The success envelope up to, not including, its closing brace:
    // {"ClientTransactionID":N,"ServerTransactionID":M,"ErrorNumber":0,"ErrorMessage":""
    // For a body written element by element (the JSON ImageArray), which cannot go through ok().
    std::string success_head() const;
    // The envelope for a refusal that is not an exception, e.g. "not yet implemented".
    Response error(std::int32_t error_number, const std::string& message) const;

    // Runs `body` and turns what it throws into the response. `label` names the
    // call in log lines, e.g. "telescope method 'slewtocoordinates'".
    //   RequestNotInterpretable -> HTTP 400 text/plain with the message;
    //   AlpacaException         -> HTTP 200, the mapped Alpaca error number;
    //   any other std::exception -> HTTP 200, DriverError (0x500) and what().
    Response run(std::string_view label, const std::function<Response()>& body) const;

private:
    const nlohmann::json* body_json() const;

    struct Raw {
        const nlohmann::json* json = nullptr;
        std::string text;
    };
    std::optional<Raw> find_raw(const std::string& name) const;

    const Request& request_;
    std::uint32_t server_tx_id_;
    std::uint32_t client_tx_id_ = 0;
    mutable std::optional<nlohmann::json> body_json_;
    mutable bool body_parsed_ = false;
    mutable std::size_t body_parse_count_ = 0;
};

}  // namespace alpacahttp

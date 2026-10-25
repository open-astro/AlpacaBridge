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

#include "device_call.h"

#include <alpacacore/alpaca_errors.h>
#include <alpacahttp/util/error_mapping.h>
#include <alpacahttp/util/log_text.h>
#include <alpacahttp/util/logging_adapter.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <istream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "../core/catalog_json.h"

namespace alpacahttp {

using catalog_json::whole_json_int;

void throw_bad_request(const std::string& message) {
    throw RequestNotInterpretable(message, alpacacore::AlpacaError::InvalidValue);
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

namespace {
std::string url_decode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            std::string hex = value.substr(i + 1, 2);
            char decoded = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            result.push_back(decoded);
            i += 2;
        } else if (value[i] == '+') {
            result.push_back(' ');
        } else {
            result.push_back(value[i]);
        }
    }
    return result;
}
}  // namespace

std::uint32_t parse_client_transaction_id(const std::string& value) {
    if (value.empty()) {
        return 0;
    }
    try {
        std::size_t pos = 0;
        long long parsed = std::stoll(value, &pos);
        if (pos != value.size()) {
            return 0;
        }
        if (parsed < 0 || parsed > static_cast<long long>(std::numeric_limits<std::uint32_t>::max())) {
            return 0;
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (...) {
        return 0;
    }
}

double parse_double_value(const std::string& raw, const std::string& param_name) {
    try {
        // #574: std::stod (libc strtod) accepts "nan", "inf", "-infinity" and
        // hex-float notation ("0x1p3", finite but not an Alpaca decimal
        // number); reject hex before parsing, and require the parsed result
        // to be finite. Both fall into the catch below via
        // std::invalid_argument, same as any other malformed value.
        if (raw.find_first_of("xX") != std::string::npos) {
            throw std::invalid_argument("hex float notation is not a valid Alpaca number");
        }
        std::size_t pos = 0;
        double value = std::stod(raw, &pos);
        if (pos != raw.size() || !std::isfinite(value)) {
            throw std::invalid_argument("trailing characters or non-finite value");
        }
        return value;
    } catch (const std::exception&) {
        throw_bad_request("Invalid value for parameter: " + param_name);
    }
}

namespace {
int parse_int_value(const std::string& raw, const std::string& param_name) {
    try {
        std::size_t pos = 0;
        long long value = std::stoll(raw, &pos);
        if (pos != raw.size()) {
            throw std::invalid_argument("trailing characters");
        }
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
            throw std::out_of_range("int range");
        }
        return static_cast<int>(value);
    } catch (const std::exception&) {
        throw_bad_request("Invalid value for parameter: " + param_name);
    }
}
}  // namespace

// A JSON-body integer parameter (#1476): accepted only as a whole number
// inside int, given as a JSON integer, a JSON number with no fraction (0.0),
// or a string parse_int_value() accepts. Anything else is a request the
// server cannot interpret (throw_bad_request, HTTP 400). The query and form paths already
// refuse "0.5" and values outside int; before this, the JSON path truncated
// 0.5 to 0 and wrapped 2^32 to 0 in all six device dispatchers.
int json_int_value(const nlohmann::json& value, const std::string& param_name) {
    if (value.is_number()) {
        if (const auto v = whole_json_int(value)) return *v;
        throw_bad_request("Invalid value for parameter: " + param_name);
    }
    if (value.is_string()) {
        return parse_int_value(value.get<std::string>(), param_name);
    }
    throw_bad_request("Invalid JSON value for parameter: " + param_name);
}

bool parse_bool_value(const std::string& raw, const std::string& param_name) {
    std::string lowered = to_lower_copy(raw);
    if (lowered == "true" || lowered == "1") {
        return true;
    }
    if (lowered == "false" || lowered == "0") {
        return false;
    }
    throw_bad_request("Invalid value for parameter: " + param_name);
}

const nlohmann::json* find_json_value(const nlohmann::json& json_obj, const std::string& key) {
    if (!json_obj.is_object()) {
        return nullptr;
    }
    auto it = json_obj.find(key);
    if (it != json_obj.end()) {
        return &it.value();
    }
    return nullptr;
}

std::optional<std::string> get_form_value(std::string_view body, const std::string& key) {
    // Alpaca spec: "Parameter names are not case sensitive, so clients and
    // drivers should be prepared for parameter names to be supplied ... with
    // any casing." Match PUT form-body parameter names case-insensitively.
    if (body.empty()) {
        return std::nullopt;
    }
    const std::string target = to_lower_copy(key);
    std::istringstream iss{std::string(body)};
    std::string pair;
    while (std::getline(iss, pair, '&')) {
        auto eq_pos = pair.find('=');
        if (eq_pos != std::string::npos) {
            std::string form_key = url_decode(pair.substr(0, eq_pos));
            std::string value = url_decode(pair.substr(eq_pos + 1));
            if (to_lower_copy(form_key) == target) {
                return value;
            }
        }
    }
    return std::nullopt;
}

namespace {

bool is_expected_not_implemented(const alpacacore::AlpacaException& e) {
    const int error_code = e.error_code();
    // NotImplemented, PropertyNotImplemented and MethodNotImplemented all map to
    // the same ASCOM code (0x400); ActionNotImplemented (0x40C) is distinct.
    return error_code == alpacacore::AlpacaError::NotImplemented ||
           error_code == alpacacore::AlpacaError::ActionNotImplemented;
}

bool is_expected_validation_error(const alpacacore::AlpacaException& e) {
    const int error_code = e.error_code();
    return error_code == alpacacore::AlpacaError::InvalidValue || error_code == alpacacore::AlpacaError::ValueNotSet;
}

// A client reading a disconnected device gets NotConnected (1031) on every poll;
// the reply is correct, so the line is client input, not a server fault.
bool is_expected_not_connected(const alpacacore::AlpacaException& e) {
    return e.error_code() == alpacacore::AlpacaError::NotConnected;
}

}  // namespace

void log_alpaca_exception(const std::string& context, const alpacacore::AlpacaException& e) {
    std::string message = context + ": " + util::escape_for_log(e.what(), 4096);
    if (is_expected_not_implemented(e) || is_expected_validation_error(e) || is_expected_not_connected(e) ||
        e.is_asked_too_early()) {
        util::log_debug(message);
    } else {
        util::log_error(message);
    }
}

DeviceCall::DeviceCall(const Request& request, std::uint32_t server_tx_id)
    : request_(request), server_tx_id_(server_tx_id) {
    // ClientTransactionID: PUT reads the body (JSON first, then form), GET reads
    // the query, anything else is 0.
    if (request_.method() == HttpMethod::PUT && !request_.body().empty()) {
        if (const auto* json = body_json()) {
            client_tx_id_ = extract_client_transaction_id(*json);
        } else if (auto value = get_form_value(request_.body(), "ClientTransactionID")) {
            client_tx_id_ = parse_client_transaction_id(*value);
        }
    } else if (request_.method() == HttpMethod::GET && request_.has_query_param("ClientTransactionID")) {
        client_tx_id_ = parse_client_transaction_id(request_.get_query_param("ClientTransactionID"));
    }
}

const nlohmann::json* DeviceCall::body_json() const {
    if (!body_parsed_) {
        body_parsed_ = true;
        if (!request_.body().empty()) {
            ++body_parse_count_;
            body_json_ = parse_json(request_.body());
        }
    }
    return body_json_ ? &*body_json_ : nullptr;
}

const nlohmann::json* DeviceCall::json_field(const std::string& name) const {
    const auto* json = body_json();
    return json != nullptr ? find_json_value(*json, name) : nullptr;
}

std::optional<std::string> DeviceCall::form_field(const std::string& name) const {
    return get_form_value(request_.body(), name);
}

// The one parameter rule: query, then the JSON body (the name, then "Value"),
// then the form body (the name, then "Value").
std::optional<DeviceCall::Raw> DeviceCall::find_raw(const std::string& name) const {
    if (request_.has_query_param(name)) {
        return Raw{nullptr, request_.get_query_param(name)};
    }
    if (const auto* json = body_json()) {
        if (const auto* value = find_json_value(*json, name)) {
            return Raw{value, {}};
        }
        if (const auto* value = find_json_value(*json, "Value")) {
            return Raw{value, {}};
        }
    }
    if (auto value = get_form_value(request_.body(), name)) {
        return Raw{nullptr, std::move(*value)};
    }
    if (auto value = get_form_value(request_.body(), "Value")) {
        return Raw{nullptr, std::move(*value)};
    }
    return std::nullopt;
}

std::optional<double> DeviceCall::find_double(const std::string& name) const {
    const auto raw = find_raw(name);
    if (!raw) return std::nullopt;
    if (raw->json == nullptr) return parse_double_value(raw->text, name);
    if (raw->json->is_number()) return raw->json->get<double>();
    if (raw->json->is_string()) return parse_double_value(raw->json->get<std::string>(), name);
    throw_bad_request("Invalid JSON value for parameter: " + name);
}

std::optional<int> DeviceCall::find_int(const std::string& name) const {
    const auto raw = find_raw(name);
    if (!raw) return std::nullopt;
    if (raw->json == nullptr) return parse_int_value(raw->text, name);
    return json_int_value(*raw->json, name);
}

std::optional<bool> DeviceCall::find_bool(const std::string& name) const {
    const auto raw = find_raw(name);
    if (!raw) return std::nullopt;
    if (raw->json == nullptr) return parse_bool_value(raw->text, name);
    if (raw->json->is_boolean()) return raw->json->get<bool>();
    if (raw->json->is_string()) return parse_bool_value(raw->json->get<std::string>(), name);
    throw_bad_request("Invalid JSON value for parameter: " + name);
}

std::optional<std::string> DeviceCall::find_string(const std::string& name) const {
    const auto raw = find_raw(name);
    if (!raw) return std::nullopt;
    if (raw->json == nullptr) return raw->text;
    if (raw->json->is_string()) return raw->json->get<std::string>();
    throw_bad_request("Invalid JSON value for parameter: " + name);
}

double DeviceCall::get_double(const std::string& name) const {
    if (auto v = find_double(name)) return *v;
    throw_bad_request("Missing parameter: " + name);
}

int DeviceCall::get_int(const std::string& name) const {
    if (auto v = find_int(name)) return *v;
    throw_bad_request("Missing parameter: " + name);
}

bool DeviceCall::get_bool(const std::string& name) const {
    if (auto v = find_bool(name)) return *v;
    throw_bad_request("Missing parameter: " + name);
}

std::string DeviceCall::get_string(const std::string& name) const {
    if (auto v = find_string(name)) return *v;
    throw_bad_request("Missing parameter: " + name);
}

Response DeviceCall::ok_void() const {
    Response response;
    response.set_body(AlpacaResponse(client_tx_id_, server_tx_id_));
    return response;
}

std::string DeviceCall::success_head() const {
    return "{\"ClientTransactionID\":" + std::to_string(client_tx_id_) +
           ",\"ServerTransactionID\":" + std::to_string(server_tx_id_) + ",\"ErrorNumber\":0,\"ErrorMessage\":\"\"";
}

Response DeviceCall::error(std::int32_t error_number, const std::string& message) const {
    Response response;
    response.set_body(make_error_response(client_tx_id_, server_tx_id_, error_number, message));
    return response;
}

Response DeviceCall::run(std::string_view label, const std::function<Response()>& body) const {
    try {
        return body();
    } catch (const RequestNotInterpretable& e) {
        // Decision 0007 rule 13: a missing or unparsable parameter is a request
        // the server cannot interpret, HTTP 400 text/plain (#1559).
        util::log_debug("Unparsable request parameter: " + util::escape_for_log(e.what()));
        Response response;
        response.set_status(400, "Bad Request");
        response.set_content_type("text/plain");
        response.set_body(std::string(e.what()));
        return response;
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in " + std::string(label), e);
        return error(util::map_error_code(e.error_code()), e.what());
    } catch (const std::exception& e) {
        util::log_error("Exception in " + std::string(label) + ": " + util::escape_for_log(e.what(), 4096));
        return error(util::ErrorCode::DRIVER_ERROR, e.what());
    }
}

}  // namespace alpacahttp

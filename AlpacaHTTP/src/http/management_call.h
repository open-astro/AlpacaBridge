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

#include <alpacahttp/json_utils.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>

#include <cstdint>
#include <exception>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace alpacahttp {

// One management API call (/management/..., issue #2486): the request plus the
// server transaction id, built once by a management handler, as DeviceCall is
// for the Device API. It owns what the handlers used to repeat:
//  - ClientTransactionID: the query string's (the name matched in any case),
//    else 0. A handler that reads the JSON body through json_body() then takes
//    a non-zero body ID over the query one, the rule the description and log
//    level writes have always had;
//  - body field lookup: json_field() returns the first accepted spelling of a
//    field that the JSON body has;
//  - the envelope: ok() and error() build the Alpaca JSON response with both ids;
//  - the exception map: failure() turns a caught exception into the error
//    envelope, an AlpacaException with its mapped number and anything else
//    DriverError (util::exception_to_error_code).
class ManagementCall {
public:
    ManagementCall(const Request& request, std::uint32_t server_tx_id);

    std::uint32_t client_tx_id() const { return client_tx_id_; }
    std::uint32_t server_tx_id() const { return server_tx_id_; }

    // The JSON body, parsed at most once; nullptr when the body is empty or not
    // JSON. A non-zero ClientTransactionID in it (any case) replaces the query one.
    const nlohmann::json* json_body() const;
    // The member of the JSON body named by the first of `spellings` it has,
    // matched exactly; nullptr when it has none or there is no JSON body.
    const nlohmann::json* json_field(std::initializer_list<const char*> spellings) const;

    template <typename T>
    Response ok(const T& value) const {
        Response response;
        response.set_body(make_success_response(client_tx_id_, server_tx_id_, value));
        return response;
    }
    Response error(std::int32_t error_number, const std::string& message) const;
    // error() with the number util::exception_to_error_code(e) gives and the
    // message `prefix` followed by e.what().
    Response failure(const std::string& prefix, const std::exception& e) const;

private:
    const Request& request_;
    std::uint32_t server_tx_id_;
    mutable std::uint32_t client_tx_id_ = 0;
    mutable std::optional<nlohmann::json> body_json_;
    mutable bool body_parsed_ = false;
};

}  // namespace alpacahttp

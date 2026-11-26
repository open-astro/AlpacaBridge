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

#include "management_call.h"

#include <alpacahttp/util/error_mapping.h>

#include "device_call.h"

namespace alpacahttp {

ManagementCall::ManagementCall(const Request& request, std::uint32_t server_tx_id)
    : request_(request), server_tx_id_(server_tx_id) {
    if (request_.has_query_param("ClientTransactionID")) {
        client_tx_id_ = parse_client_transaction_id(request_.get_query_param("ClientTransactionID"));
    }
}

const nlohmann::json* ManagementCall::json_body() const {
    if (!body_parsed_) {
        body_parsed_ = true;
        if (!request_.body().empty()) {
            body_json_ = parse_json(request_.body());
            if (body_json_) {
                if (const auto body_tx_id = extract_client_transaction_id(*body_json_); body_tx_id != 0) {
                    client_tx_id_ = body_tx_id;
                }
            }
        }
    }
    return body_json_ ? &*body_json_ : nullptr;
}

const nlohmann::json* ManagementCall::json_field(std::initializer_list<const char*> spellings) const {
    const auto* body = json_body();
    if (body == nullptr) {
        return nullptr;
    }
    for (const char* spelling : spellings) {
        if (const auto* value = find_json_value(*body, spelling)) {
            return value;
        }
    }
    return nullptr;
}

Response ManagementCall::error(std::int32_t error_number, const std::string& message) const {
    Response response;
    response.set_body(make_error_response(client_tx_id_, server_tx_id_, error_number, message));
    return response;
}

Response ManagementCall::failure(const std::string& prefix, const std::exception& e) const {
    return error(util::exception_to_error_code(e), prefix + e.what());
}

}  // namespace alpacahttp

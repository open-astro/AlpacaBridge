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

#include "response.h"
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <optional>

namespace alpacahttp {

// Convert AlpacaResponse to JSON
nlohmann::json to_json(const AlpacaResponse& response);

// Parse JSON to AlpacaResponse
AlpacaResponse from_json(const nlohmann::json& j);

// Parse JSON from string
std::optional<nlohmann::json> parse_json(std::string_view json_str);

// Extract transaction ID from JSON request
std::uint32_t extract_client_transaction_id(const nlohmann::json& j);

// Build error response
AlpacaResponse make_error_response(
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id,
    std::int32_t error_number,
    const std::string& error_message
);

// Build success response with value
template<typename T>
AlpacaResponse make_success_response(
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id,
    const T& value
) {
    AlpacaResponse response(client_tx_id, server_tx_id);
    response.value = value;
    return response;
}

} // namespace alpacahttp


// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://github.com/open-astro/AlpacaHTTP/blob/main/LICENSE
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

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


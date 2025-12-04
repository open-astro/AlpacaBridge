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

#include <alpacahttp/json_utils.h>
#include <sstream>

namespace alpacahttp {

nlohmann::json to_json(const AlpacaResponse& response) {
    nlohmann::json j;
    j["ClientTransactionID"] = response.client_transaction_id;
    j["ServerTransactionID"] = response.server_transaction_id;
    j["ErrorNumber"] = response.error_number;
    j["ErrorMessage"] = response.error_message;

    if (response.value.has_value()) {
        std::visit([&j](const auto& val) {
            j["Value"] = val;
        }, response.value.value());
    }

    return j;
}

AlpacaResponse from_json(const nlohmann::json& j) {
    AlpacaResponse response;

    if (j.contains("ClientTransactionID")) {
        response.client_transaction_id = j["ClientTransactionID"].get<std::uint32_t>();
    }
    if (j.contains("ServerTransactionID")) {
        response.server_transaction_id = j["ServerTransactionID"].get<std::uint32_t>();
    }
    if (j.contains("ErrorNumber")) {
        response.error_number = j["ErrorNumber"].get<std::int32_t>();
    }
    if (j.contains("ErrorMessage")) {
        response.error_message = j["ErrorMessage"].get<std::string>();
    }
    if (j.contains("Value")) {
        const auto& value = j["Value"];
        if (value.is_boolean()) {
            response.value = value.get<bool>();
        } else if (value.is_number_integer()) {
            response.value = value.get<std::int32_t>();
        } else if (value.is_number_float()) {
            response.value = value.get<double>();
        } else if (value.is_string()) {
            response.value = value.get<std::string>();
        }
    }

    return response;
}

std::optional<nlohmann::json> parse_json(std::string_view json_str) {
    try {
        return nlohmann::json::parse(json_str);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::uint32_t extract_client_transaction_id(const nlohmann::json& j) {
    if (j.contains("ClientTransactionID")) {
        return j["ClientTransactionID"].get<std::uint32_t>();
    }
    return 0;
}

AlpacaResponse make_error_response(
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id,
    std::int32_t error_number,
    const std::string& error_message
) {
    AlpacaResponse response(client_tx_id, server_tx_id);
    response.error_number = error_number;
    response.error_message = error_message;
    return response;
}

} // namespace alpacahttp


// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacahttp/json_utils.h>
#include <sstream>
#include <type_traits>
#include <limits>

namespace alpacahttp {

nlohmann::json to_json(const AlpacaResponse& response) {
    nlohmann::json j;
    j["ClientTransactionID"] = response.client_transaction_id;
    j["ServerTransactionID"] = response.server_transaction_id;
    j["ErrorNumber"] = response.error_number;
    j["ErrorMessage"] = response.error_message;

    if (response.value.has_value()) {
        std::visit([&j](const auto& val) {
            // If value is a string that looks like JSON, parse it
            if constexpr (std::is_same_v<std::decay_t<decltype(val)>, std::string>) {
                // Try to parse as JSON - if it's valid JSON, use the parsed value
                // Otherwise, use the string as-is
                try {
                    auto parsed = nlohmann::json::parse(val);
                    j["Value"] = parsed;
                } catch (const nlohmann::json::exception&) {
                    // Not valid JSON, use as string
                    j["Value"] = val;
                }
            } else {
                j["Value"] = val;
            }
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
    const nlohmann::json* candidate = nullptr;
    if (j.contains("ClientTransactionID")) {
        candidate = &j["ClientTransactionID"];
    }

    if (candidate) {
        try {
            if (candidate->is_number_integer()) {
                auto value = candidate->get<std::int64_t>();
                if (value < 0 || value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
                    return 0;
                }
                return static_cast<std::uint32_t>(value);
            }
            if (candidate->is_number_unsigned()) {
                auto value = candidate->get<std::uint64_t>();
                if (value > std::numeric_limits<std::uint32_t>::max()) {
                    return 0;
                }
                return static_cast<std::uint32_t>(value);
            }
        } catch (const nlohmann::json::exception&) {
            return 0;
        }
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

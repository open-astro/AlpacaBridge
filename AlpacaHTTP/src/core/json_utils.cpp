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

#include <alpacahttp/json_utils.h>
#include <sstream>
#include <type_traits>
#include <limits>
#include <algorithm>
#include <cctype>

namespace alpacahttp {

nlohmann::json to_json(const AlpacaResponse& response) {
    nlohmann::json j;
    j["ClientTransactionID"] = response.client_transaction_id;
    j["ServerTransactionID"] = response.server_transaction_id;
    j["ErrorNumber"] = response.error_number;
    j["ErrorMessage"] = response.error_message;

    if (response.value.has_value()) {
        // value already holds structured JSON; emit it verbatim. No string
        // re-parsing — a string Value stays a string (correct ASCOM typing).
        j["Value"] = *response.value;
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
        response.value = j.at("Value");
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
    if (!candidate && j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string key = it.key();
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (key == "clienttransactionid") {
                candidate = &it.value();
                break;
            }
        }
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

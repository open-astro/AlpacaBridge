// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

#include <alpacahttp/router.h>
#include <alpacahttp/json_utils.h>
#include <alpacahttp/util/error_mapping.h>
#include <alpacahttp/util/logging_adapter.h>
#include <sstream>
#include <regex>
#include <stdexcept>

namespace alpacahttp {

// Forward declaration - placeholder for AlpacaCore integration
class DeviceManager {
    // TODO: Replace with actual AlpacaCore::DeviceManager
};

Router::Router() = default;
Router::~Router() = default;

void Router::set_device_manager(std::shared_ptr<DeviceManager> device_manager) {
    device_manager_ = device_manager;
}

Response Router::route(const Request& request, std::uint32_t server_transaction_id) {
    Response response;
    response.set_content_type("application/json");

    try {
        RouteMatch match = parse_route(request.path());

        if (match.is_management) {
            return handle_management(request, match, server_transaction_id);
        } else {
            return handle_device(request, match, server_transaction_id);
        }
    } catch (const std::exception& e) {
        log_error("Router error: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            0, server_transaction_id,
            util::ErrorCode::DRIVER_ERROR,
            "Internal server error: " + std::string(e.what())
        );
        response.set_body(alpaca_response);
        return response;
    }
}

RouteMatch Router::parse_route(const std::string& path) {
    RouteMatch match;

    // Management endpoints
    if (path == "/management/v1/description") {
        match.is_management = true;
        match.management_endpoint = "description";
        return match;
    }
    if (path == "/management/v1/configureddevices") {
        match.is_management = true;
        match.management_endpoint = "configureddevices";
        return match;
    }

    // Device API: /api/v1/{devicetype}/{devicenumber}/{method}
    std::regex device_regex(R"(/api/v1/([^/]+)/(\d+)/([^/?]+))");
    std::smatch matches;
    if (std::regex_match(path, matches, device_regex)) {
        match.device_type = matches[1].str();
        match.device_number = static_cast<std::uint32_t>(std::stoul(matches[2].str()));
        match.method_name = matches[3].str();
        return match;
    }

    // Not found
    return match;
}

Response Router::handle_management(const Request& request, const RouteMatch& match, std::uint32_t server_tx_id) {
    if (match.management_endpoint == "description") {
        return handle_description(request, server_tx_id);
    } else if (match.management_endpoint == "configureddevices") {
        return handle_configured_devices(request, server_tx_id);
    }

    Response response;
    AlpacaResponse alpaca_response = make_error_response(
        0, server_tx_id,
        util::ErrorCode::INVALID_VALUE,
        "Unknown management endpoint"
    );
    response.set_body(alpaca_response);
    return response;
}

Response Router::handle_description(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    
    // TODO: Get actual server description from config
    nlohmann::json desc;
    desc["ServerName"] = "AlpacaHTTP";
    desc["Manufacturer"] = "AlpacaHTTP";
    desc["ManufacturerVersion"] = "0.1.0";
    desc["Location"] = "";

    AlpacaResponse alpaca_response(0, server_tx_id);
    alpaca_response.value = desc.dump();
    response.set_body(alpaca_response);
    return response;
}

Response Router::handle_configured_devices(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    
    // TODO: Get actual configured devices from AlpacaCore
    nlohmann::json devices = nlohmann::json::array();

    AlpacaResponse alpaca_response(0, server_tx_id);
    alpaca_response.value = devices.dump();
    response.set_body(alpaca_response);
    return response;
}

Response Router::handle_device(const Request& request, const RouteMatch& match, std::uint32_t server_tx_id) {
    Response response;

    // Extract client transaction ID from request
    std::uint32_t client_tx_id = 0;
    if (request.method() == HttpMethod::PUT && !request.body().empty()) {
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            client_tx_id = extract_client_transaction_id(*json_opt);
        }
    } else if (request.method() == HttpMethod::GET) {
        if (request.has_query_param("ClientTransactionID")) {
            try {
                client_tx_id = static_cast<std::uint32_t>(std::stoul(request.get_query_param("ClientTransactionID")));
            } catch (...) {
                // Invalid transaction ID
            }
        }
    }

    try {
        // TODO: Call AlpacaCore device method
        // For now, return not implemented
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Device method not yet implemented"
        );
        response.set_body(alpaca_response);
    } catch (const std::exception& e) {
        log_error("Device error: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
        response.set_body(alpaca_response);
    }

    return response;
}

} // namespace alpacahttp


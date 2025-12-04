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

#include <alpacahttp/router.h>
#include <alpacahttp/json_utils.h>
#include <alpacahttp/util/error_mapping.h>
#include <alpacahttp/util/logging_adapter.h>
#include <alpacacore/device_registry.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/alpaca_defs.h>
#include <sstream>
#include <regex>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace alpacahttp {

Router::Router() = default;
Router::~Router() = default;

void Router::set_management_driver(std::shared_ptr<alpacacore::ManagementDriver> mgmt_driver) {
    management_driver_ = mgmt_driver;
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
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        try {
            client_tx_id = static_cast<std::uint32_t>(std::stoul(request.get_query_param("ClientTransactionID")));
        } catch (...) {
            // Invalid transaction ID
        }
    }

    try {
        if (!management_driver_) {
            // Fallback to default values if no management driver is set
            nlohmann::json desc;
            desc["ServerName"] = "AlpacaHTTP";
            desc["Manufacturer"] = "AlpacaHTTP";
            desc["ManufacturerVersion"] = "0.1.0";
            desc["Location"] = "";

            AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
            alpaca_response.value = desc.dump();
            response.set_body(alpaca_response);
            return response;
        }

        nlohmann::json desc;
        desc["ServerName"] = management_driver_->get_name();
        desc["Manufacturer"] = management_driver_->get_manufacturer();
        desc["ManufacturerVersion"] = management_driver_->get_manufacturer_version();
        desc["Location"] = management_driver_->get_location();

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = desc.dump();
        response.set_body(alpaca_response);
    } catch (const std::exception& e) {
        log_error("Error getting description: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
        response.set_body(alpaca_response);
    }

    return response;
}

Response Router::handle_configured_devices(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        try {
            client_tx_id = static_cast<std::uint32_t>(std::stoul(request.get_query_param("ClientTransactionID")));
        } catch (...) {
            // Invalid transaction ID
        }
    }

    try {
        // Get devices from AlpacaCore registry
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto capabilities = registry.get_all_device_capabilities();

        nlohmann::json devices = nlohmann::json::array();
        for (const auto& cap : capabilities) {
            nlohmann::json device;
            device["DeviceName"] = cap.name;
            device["DeviceType"] = alpacacore::device_type_to_string(cap.type);
            device["DeviceNumber"] = cap.device_number;
            device["UniqueID"] = cap.unique_id;
            devices.push_back(device);
        }

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = devices.dump();
        response.set_body(alpaca_response);
    } catch (const std::exception& e) {
        log_error("Error getting configured devices: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
        response.set_body(alpaca_response);
    }

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
        // Convert device type string to enum
        alpacacore::DeviceType device_type = string_to_device_type(match.device_type);
        
        // Get device from registry
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto device = registry.get_device(device_type, static_cast<int>(match.device_number));
        
        if (!device) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Device not found: " + match.device_type + " #" + std::to_string(match.device_number)
            );
            response.set_body(alpaca_response);
            return response;
        }

        // Dispatch the method call
        return dispatch_device_method(device, match.method_name, request, client_tx_id, server_tx_id);
        
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

alpacacore::DeviceType Router::string_to_device_type(const std::string& type_str) const {
    std::string lower_type = type_str;
    std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower_type == "camera") return alpacacore::DeviceType::Camera;
    if (lower_type == "telescope" || lower_type == "mount") return alpacacore::DeviceType::Telescope;
    if (lower_type == "filterwheel") return alpacacore::DeviceType::FilterWheel;
    if (lower_type == "focuser") return alpacacore::DeviceType::Focuser;
    if (lower_type == "rotator") return alpacacore::DeviceType::Rotator;
    if (lower_type == "dome") return alpacacore::DeviceType::Dome;
    if (lower_type == "shutter") return alpacacore::DeviceType::Shutter;
    if (lower_type == "switch") return alpacacore::DeviceType::Switch;
    if (lower_type == "covercalibrator") return alpacacore::DeviceType::CoverCalibrator;
    if (lower_type == "observingconditions") return alpacacore::DeviceType::ObservingConditions;
    if (lower_type == "safetymonitor") return alpacacore::DeviceType::SafetyMonitor;

    throw std::runtime_error("Unknown device type: " + type_str);
}

Response Router::dispatch_device_method(
    std::shared_ptr<alpacacore::AlpacaDriver> device,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;
    
    try {
        // Handle common AlpacaDriver methods
        if (method_name == "name") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, device->get_name());
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "description") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, device->get_description());
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "driverinfo") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, device->get_driver_info());
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "driverversion") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, device->get_driver_version());
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "interfaceversion") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, static_cast<std::int32_t>(device->get_interface_version()));
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "connected") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, device->get_connected());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                // Parse JSON body for connected value
                auto json_opt = parse_json(request.body());
                if (!json_opt) {
                    throw std::runtime_error("Invalid JSON in request body");
                }
                
                bool connected = false;
                if (json_opt->contains("Connected")) {
                    connected = json_opt->at("Connected").get<bool>();
                } else if (json_opt->contains("Value")) {
                    connected = json_opt->at("Value").get<bool>();
                } else {
                    throw std::runtime_error("Missing 'Connected' or 'Value' in request");
                }
                
                device->set_connected(connected);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "supportedactions") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, device->get_supported_actions());
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "action") {
            if (request.method() == HttpMethod::PUT) {
                auto json_opt = parse_json(request.body());
                if (!json_opt) {
                    throw std::runtime_error("Invalid JSON in request body");
                }
                
                std::string action_name;
                std::string action_parameters = "{}";
                
                if (json_opt->contains("Action")) {
                    action_name = json_opt->at("Action").get<std::string>();
                } else {
                    throw std::runtime_error("Missing 'Action' in request");
                }
                
                if (json_opt->contains("Parameters")) {
                    action_parameters = json_opt->at("Parameters").dump();
                }
                
                std::string result = device->action(action_name, action_parameters);
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, result);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "commandblind") {
            if (request.method() == HttpMethod::PUT) {
                auto json_opt = parse_json(request.body());
                if (!json_opt) {
                    throw std::runtime_error("Invalid JSON in request body");
                }
                
                std::string command;
                bool raw = false;
                
                if (json_opt->contains("Command")) {
                    command = json_opt->at("Command").get<std::string>();
                } else {
                    throw std::runtime_error("Missing 'Command' in request");
                }
                
                if (json_opt->contains("Raw")) {
                    raw = json_opt->at("Raw").get<bool>();
                }
                
                device->command_blind(command, raw);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "commandbool") {
            if (request.method() == HttpMethod::PUT) {
                auto json_opt = parse_json(request.body());
                if (!json_opt) {
                    throw std::runtime_error("Invalid JSON in request body");
                }
                
                std::string command;
                bool raw = false;
                
                if (json_opt->contains("Command")) {
                    command = json_opt->at("Command").get<std::string>();
                } else {
                    throw std::runtime_error("Missing 'Command' in request");
                }
                
                if (json_opt->contains("Raw")) {
                    raw = json_opt->at("Raw").get<bool>();
                }
                
                bool result = device->command_bool(command, raw);
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, result);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "commandstring") {
            if (request.method() == HttpMethod::PUT) {
                auto json_opt = parse_json(request.body());
                if (!json_opt) {
                    throw std::runtime_error("Invalid JSON in request body");
                }
                
                std::string command;
                bool raw = false;
                
                if (json_opt->contains("Command")) {
                    command = json_opt->at("Command").get<std::string>();
                } else {
                    throw std::runtime_error("Missing 'Command' in request");
                }
                
                if (json_opt->contains("Raw")) {
                    raw = json_opt->at("Raw").get<bool>();
                }
                
                std::string result = device->command_string(command, raw);
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, result);
                response.set_body(alpaca_response);
                return response;
            }
        }
        
        // Try device-specific methods (e.g., CameraDriver)
        // This is a simplified approach - a full implementation would use
        // reflection or a method dispatch table for each device type
        DeviceType device_type = device->get_device_type();
        
        // For now, return not implemented for device-specific methods
        // TODO: Implement device-specific method dispatch
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for device type " +
            std::string(alpacacore::device_type_to_string(device_type))
        );
        response.set_body(alpaca_response);
        return response;
        
    } catch (const alpacacore::AlpacaException& e) {
        log_error("AlpacaException in device method: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::DRIVER_ERROR,
            std::string(e.what())
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        log_error("Exception in device method: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
        response.set_body(alpaca_response);
        return response;
    }
}

} // namespace alpacahttp


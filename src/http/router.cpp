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

#include <alpacahttp/router.h>
#include <alpacahttp/json_utils.h>
#include <alpacahttp/util/error_mapping.h>
#include <alpacahttp/util/logging_adapter.h>
#include <alpacacore/device_registry.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/alpaca_defs.h>
#include <alpacacore/util/logging.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <regex>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <vector>
#include <chrono>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <thread>
#include <optional>
#include <array>
#ifdef ALPACACORE_ENABLE_IOPTRON
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>
#endif

namespace {

using alpacacore::logging::LogLevel;

const std::filesystem::path kPersistedDevicesFile = std::filesystem::path("config") / "registered_devices.json";

const std::array<std::pair<const char*, LogLevel>, 6> kLogLevelMap = {{
    {"TRACE", LogLevel::Trace},
    {"DEBUG", LogLevel::Debug},
    {"INFO", LogLevel::Info},
    {"WARN", LogLevel::Warn},
    {"WARNING", LogLevel::Warn},
    {"ERROR", LogLevel::Error},
}};

std::string normalize_level_string(std::string level) {
    std::transform(level.begin(), level.end(), level.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return level;
}

std::string log_level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARNING";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Critical: return "CRITICAL";
        default: return "INFO";
    }
}

std::optional<LogLevel> parse_log_level_string(const std::string& input) {
    std::string level = normalize_level_string(input);
    if (level == "CRITICAL") {
        return LogLevel::Critical;
    }
    for (const auto& entry : kLogLevelMap) {
        if (level == entry.first) {
            return entry.second;
        }
    }
    if (level == "FATAL") {
        return LogLevel::Critical;
    }
    return std::nullopt;
}

nlohmann::json make_log_level_payload() {
    nlohmann::json payload;
    payload["Level"] = log_level_to_string(alpacacore::logging::get_log_level());
    payload["SupportedLevels"] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"};
    return payload;
}

} // namespace

namespace alpacahttp {

Router::Router() {
    load_persisted_devices();
}
Router::~Router() = default;

void Router::set_management_driver(std::shared_ptr<alpacacore::ManagementDriver> mgmt_driver) {
    management_driver_ = mgmt_driver;
}

void Router::set_shutdown_callback(std::function<void()> callback) {
    shutdown_callback_ = callback;
}

Response Router::route(const Request& request, std::uint32_t server_transaction_id) {
    Response response;
    response.set_content_type("application/json");

    // Log incoming requests for debugging
    std::string method_str = (request.method() == HttpMethod::GET) ? "GET" : 
                           (request.method() == HttpMethod::POST) ? "POST" : 
                           (request.method() == HttpMethod::PUT) ? "PUT" : "UNKNOWN";
    util::log_debug("HTTP " + method_str + " " + request.path());

    try {
        // Handle static file requests (web UI)
        if (request.path().find("/web/") == 0) {
            return handle_static_file(request);
        }

        // Handle root path - serve web UI
        if (request.path() == "/" || request.path() == "") {
            if (request.method() == HttpMethod::GET) {
                return handle_static_file(request);
            }
        }

        // Handle driver setup pages: /setup/v1/{devicetype}/{devicenumber}/setup
        if (request.path().find("/setup/v1/") == 0) {
            return handle_setup(request, server_transaction_id);
        }

        RouteMatch match = parse_route(request.path());

        // Check if route is valid
        if (!match.is_management && match.device_type.empty()) {
            // No valid route matched - return 404
            response.set_status(404, "Not Found");
            std::uint32_t client_tx_id = 0;
            if (request.has_query_param("ClientTransactionID")) {
                try {
                    client_tx_id = static_cast<std::uint32_t>(std::stoul(request.get_query_param("ClientTransactionID")));
                } catch (...) {
                    // Invalid transaction ID
                }
            }
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_transaction_id,
                util::ErrorCode::INVALID_VALUE,
                "Endpoint not found: " + request.path()
            );
            response.set_body(alpaca_response);
            return response;
        }

        if (match.is_management) {
            return handle_management(request, match, server_transaction_id);
        } else {
            return handle_device(request, match, server_transaction_id);
        }
    } catch (const std::exception& e) {
        util::log_error("Router error: " + std::string(e.what()));
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

    // Root endpoint - return helpful information (JSON API)
    if (path == "/api" || path == "/api/") {
        match.is_management = true;
        match.management_endpoint = "root";
        return match;
    }

    // Management endpoints (support both /management/v1/... and /management/... for compatibility)
    if (path == "/management/v1/description" || path == "/management/description") {
        match.is_management = true;
        match.management_endpoint = "description";
        return match;
    }
    if (path == "/management/v1/apiversions" || path == "/management/apiversions") {
        match.is_management = true;
        match.management_endpoint = "apiversions";
        return match;
    }
    if (path == "/management/v1/configureddevices" || path == "/management/configureddevices") {
        match.is_management = true;
        match.management_endpoint = "configureddevices";
        return match;
    }
    if (path == "/management/v1/configuredevice" || path == "/management/configuredevice") {
        match.is_management = true;
        match.management_endpoint = "configuredevice";
        return match;
    }
    if (path == "/management/v1/removedevice" || path == "/management/removedevice") {
        match.is_management = true;
        match.management_endpoint = "removedevice";
        return match;
    }
    if (path == "/management/v1/loglevel" || path == "/management/loglevel") {
        match.is_management = true;
        match.management_endpoint = "loglevel";
        return match;
    }
    if (path == "/management/v1/shutdown" || path == "/management/shutdown") {
        match.is_management = true;
        match.management_endpoint = "shutdown";
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
    if (match.management_endpoint == "root") {
        return handle_root(request, server_tx_id);
    } else if (match.management_endpoint == "description") {
        return handle_description(request, server_tx_id);
    } else if (match.management_endpoint == "apiversions") {
        return handle_api_versions(request, server_tx_id);
    } else if (match.management_endpoint == "configureddevices") {
        return handle_configured_devices(request, server_tx_id);
    } else if (match.management_endpoint == "configuredevice") {
        return handle_configure_device(request, server_tx_id);
    } else if (match.management_endpoint == "removedevice") {
        return handle_remove_device(request, server_tx_id);
    } else if (match.management_endpoint == "loglevel") {
        return handle_log_level(request, server_tx_id);
    } else if (match.management_endpoint == "shutdown") {
        return handle_shutdown(request, server_tx_id);
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
        util::log_error("Error getting description: " + std::string(e.what()));
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
        load_persisted_devices();

        auto find_config = [this](const std::string& device_type, int device_number) -> const nlohmann::json* {
            std::string target_type = device_type;
            std::transform(target_type.begin(), target_type.end(), target_type.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const auto& entry : persisted_devices_) {
                std::string entry_type = entry.value("deviceType", "");
                std::transform(entry_type.begin(), entry_type.end(), entry_type.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (entry_type == target_type &&
                    entry.value("deviceNumber", -1) == device_number) {
                    return &entry;
                }
            }
            return nullptr;
        };

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
            
            if (const auto* config = find_config(device["DeviceType"].get<std::string>(), cap.device_number)) {
                device["Vendor"] = config->value("vendor", "");
                device["Config"] = *config;
            }
            devices.push_back(device);
        }

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = devices.dump();
        response.set_body(alpaca_response);
    } catch (const std::exception& e) {
        util::log_error("Error getting configured devices: " + std::string(e.what()));
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
        // Try JSON first
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            client_tx_id = extract_client_transaction_id(*json_opt);
        } else {
            // Try form-encoded (format: Connected=true&ClientID=1&ClientTransactionID=1)
            std::istringstream iss(request.body());
            std::string pair;
            while (std::getline(iss, pair, '&')) {
                auto eq_pos = pair.find('=');
                if (eq_pos != std::string::npos) {
                    std::string key = pair.substr(0, eq_pos);
                    std::string value = pair.substr(eq_pos + 1);
                    if (key == "ClientTransactionID") {
                        try {
                            client_tx_id = static_cast<std::uint32_t>(std::stoul(value));
                        } catch (...) {
                            // Invalid transaction ID
                        }
                        break;
                    }
                }
            }
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
        util::log_error("Device error: " + std::string(e.what()));
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
                bool connected = false;
                bool found = false;
                
                // Try parsing as JSON first
                auto json_opt = parse_json(request.body());
                if (json_opt) {
                    if (json_opt->contains("Connected")) {
                        connected = json_opt->at("Connected").get<bool>();
                        found = true;
                    } else if (json_opt->contains("Value")) {
                        connected = json_opt->at("Value").get<bool>();
                        found = true;
                    }
                }
                
                // If JSON parsing failed or didn't contain Connected/Value, try form-encoded
                if (!found && !request.body().empty()) {
                    // Parse form-encoded body (format: Connected=true&ClientID=1&ClientTransactionID=1)
                    std::istringstream iss(request.body());
                    std::string pair;
                    while (std::getline(iss, pair, '&')) {
                        auto eq_pos = pair.find('=');
                        if (eq_pos != std::string::npos) {
                            std::string key = pair.substr(0, eq_pos);
                            std::string value = pair.substr(eq_pos + 1);
                            
                            if (key == "Connected" || key == "Value") {
                                std::transform(value.begin(), value.end(), value.begin(), ::tolower);
                                connected = (value == "true" || value == "1");
                                found = true;
                                break;
                            }
                        }
                    }
                }
                
                if (!found) {
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
        
        // Try device-specific methods
        alpacacore::DeviceType device_type = device->get_device_type();
        
        // Handle telescope-specific methods
        if (device_type == alpacacore::DeviceType::Telescope) {
            auto telescope = std::dynamic_pointer_cast<alpacacore::TelescopeDriver>(device);
            if (telescope) {
                return dispatch_telescope_method(telescope, method_name, request, client_tx_id, server_tx_id);
            }
        }
        
        // For other device types or if cast failed, return not implemented
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for device type " +
            std::string(alpacacore::device_type_to_string(device_type))
        );
        response.set_body(alpaca_response);
        return response;
        
    } catch (const alpacacore::AlpacaException& e) {
        util::log_error("AlpacaException in device method: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::map_error_code(e.error_code()),
            std::string(e.what())
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in device method: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::dispatch_telescope_method(
    std::shared_ptr<alpacacore::TelescopeDriver> telescope,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;
    
    try {
        // Helper function to URL decode a string
        auto url_decode = [](const std::string& str) -> std::string {
            std::string result;
            result.reserve(str.size());
            for (size_t i = 0; i < str.size(); ++i) {
                if (str[i] == '%' && i + 2 < str.size()) {
                    int value;
                    std::istringstream iss(str.substr(i + 1, 2));
                    if (iss >> std::hex >> value) {
                        result += static_cast<char>(value);
                        i += 2;
                    } else {
                        result += str[i];
                    }
                } else if (str[i] == '+') {
                    result += ' ';
                } else {
                    result += str[i];
                }
            }
            return result;
        };
        
        // Helper function to compare strings case-insensitively
        auto case_insensitive_compare = [](const std::string& a, const std::string& b) -> bool {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) != 
                    std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        };
        
        // Helper function to parse form-encoded parameter from body (case-insensitive)
        auto parse_form_param = [&](const std::string& param_name) -> std::string {
            if (request.body().empty()) {
                return "";
            }
            std::istringstream iss(request.body());
            std::string pair;
            while (std::getline(iss, pair, '&')) {
                auto eq_pos = pair.find('=');
                if (eq_pos != std::string::npos) {
                    std::string key = pair.substr(0, eq_pos);
                    std::string value = pair.substr(eq_pos + 1);
                    // Case-insensitive comparison for form parameters
                    if (case_insensitive_compare(key, param_name)) {
                        return url_decode(value);
                    }
                }
            }
            return "";
        };
        
        // Helper function to parse double from query param, JSON body, or form-encoded body
        auto parse_double = [&](const std::string& param_name) -> double {
            // Try query parameter first
            if (request.has_query_param(param_name)) {
                try {
                    return std::stod(request.get_query_param(param_name));
                } catch (const std::exception&) {
                    throw std::runtime_error("Invalid value for parameter: " + param_name);
                }
            }
            // Try JSON body
            if (!request.body().empty()) {
                auto json_opt = parse_json(request.body());
                if (json_opt && json_opt->contains(param_name)) {
                    try {
                        return json_opt->at(param_name).get<double>();
                    } catch (const std::exception&) {
                        throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                    }
                }
            }
            // Try form-encoded
            std::string form_value = parse_form_param(param_name);
            if (!form_value.empty()) {
                try {
                    return std::stod(form_value);
                } catch (const std::exception&) {
                    throw std::runtime_error("Invalid form value for parameter: " + param_name);
                }
            }
            throw std::runtime_error("Missing parameter: " + param_name);
        };
        
        // Helper function to parse int from query param, JSON body, or form-encoded body
        auto parse_int = [&](const std::string& param_name) -> int {
            // Try query parameter first
            if (request.has_query_param(param_name)) {
                try {
                    return std::stoi(request.get_query_param(param_name));
                } catch (const std::exception&) {
                    throw std::runtime_error("Invalid value for parameter: " + param_name);
                }
            }
            // Try JSON body
            if (!request.body().empty()) {
                auto json_opt = parse_json(request.body());
                if (json_opt && json_opt->contains(param_name)) {
                    try {
                        return json_opt->at(param_name).get<int>();
                    } catch (const std::exception&) {
                        throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                    }
                }
            }
            // Try form-encoded
            std::string form_value = parse_form_param(param_name);
            if (!form_value.empty()) {
                try {
                    return std::stoi(form_value);
                } catch (const std::exception&) {
                    throw std::runtime_error("Invalid form value for parameter: " + param_name);
                }
            }
            throw std::runtime_error("Missing parameter: " + param_name);
        };
        
        // Helper function to parse bool from query param, JSON body, or form-encoded body
        auto parse_bool = [&](const std::string& param_name) -> bool {
            // Try query parameter first
            if (request.has_query_param(param_name)) {
                std::string val = request.get_query_param(param_name);
                std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                return (val == "true" || val == "1");
            }
            
            // Try JSON body
            auto json_opt = parse_json(request.body());
            if (json_opt && json_opt->contains(param_name)) {
                return json_opt->at(param_name).get<bool>();
            }
            
            // Try form-encoded body (e.g., "Tracking=true" or "Tracking=1")
            if (!request.body().empty()) {
                std::istringstream iss(request.body());
                std::string pair;
                while (std::getline(iss, pair, '&')) {
                    auto eq_pos = pair.find('=');
                    if (eq_pos != std::string::npos) {
                        std::string key = pair.substr(0, eq_pos);
                        std::string value = pair.substr(eq_pos + 1);
                        
                        if (key == param_name || key == "Value") {
                            std::transform(value.begin(), value.end(), value.begin(), ::tolower);
                            return (value == "true" || value == "1");
                        }
                    }
                }
            }
            
            throw std::runtime_error("Missing parameter: " + param_name);
        };
        
        // Helper function to parse string from query param or JSON body
        auto parse_string = [&](const std::string& param_name) -> std::string {
            if (request.has_query_param(param_name)) {
                return request.get_query_param(param_name);
            }
            auto json_opt = parse_json(request.body());
            if (json_opt && json_opt->contains(param_name)) {
                return json_opt->at(param_name).get<std::string>();
            }
            throw std::runtime_error("Missing parameter: " + param_name);
        };
        
        // GET-only boolean properties
        if (request.method() == HttpMethod::GET) {
            if (method_name == "cansetrightascensionrate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_set_right_ascension_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansetdeclinationrate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_set_declination_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansetguiderates") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_set_guide_rates());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansettracking") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_set_tracking());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansetpark") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_set_park());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansetpierside") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_set_pier_side());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canfindhome") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_find_home());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canpark") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_park());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canpulseguide") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_pulse_guide());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canslew") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_slew());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canslewasync") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_slew_async());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansync") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_sync());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canunpark") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_unpark());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canslewaltaz") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, false);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canslewaltazasync") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, false);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansyncaltaz") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, false);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "alignmentmode") {
                int mode = static_cast<int>(telescope->get_alignment_mode());
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, mode);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "equatorialsystem") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, 0);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "doesrefraction") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, false);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "ispulseguiding") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, false);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewsettletime") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, 0.0);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "utcdate") {
                auto utc = telescope->get_utc_date();
                auto time_t = std::chrono::system_clock::to_time_t(utc);
                std::ostringstream oss;
                oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    utc.time_since_epoch()) % 1000;
                oss << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, oss.str());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "axisrates") {
                int axis = 0;
                if (request.has_query_param("Axis")) {
                    axis = std::stoi(request.get_query_param("Axis"));
                }
                // AxisRates must return an array of rate objects, even if only one range
                nlohmann::json rates_array = nlohmann::json::array();
                nlohmann::json rate_obj;
                rate_obj["Minimum"] = 0.0;
                rate_obj["Maximum"] = 3.0;
                rates_array.push_back(rate_obj);
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, rates_array.dump());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canmoveaxis") {
                int axis = 0;
                if (request.has_query_param("Axis")) {
                    axis = std::stoi(request.get_query_param("Axis"));
                }
                bool can_move = telescope->get_can_move_axis(axis);
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, can_move);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "destinationsideofpier") {
                double ra = parse_double("RightAscension");
                double dec = parse_double("Declination");
                int side = telescope->get_side_of_pier();
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, side);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "athome") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_at_home());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "atpark") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_at_park());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewing") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_slewing());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "tracking") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_tracking());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "altitude") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_altitude());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "azimuth") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_azimuth());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "declination") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_declination());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "rightascension") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_right_ascension());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "declinationrate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_declination_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "rightascensionrate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_right_ascension_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "trackingrate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_tracking_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "trackingrates") {
                auto rates = telescope->get_tracking_rates();
                nlohmann::json rates_array = nlohmann::json::array();
                for (int rate : rates) {
                    rates_array.push_back(rate);
                }
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, rates_array.dump());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "targetdeclination") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_target_declination());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "targetrightascension") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_target_right_ascension());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "siderealtime") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_sidereal_time());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "siteelevation") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_site_elevation());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "sitelatitude") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_site_latitude());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "sitelongitude") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_site_longitude());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "focallength") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_focal_length());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "aperturediameter") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_aperture_diameter());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "aperturearea") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_aperture_area());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "sideofpier") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_side_of_pier());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "guideratedeclination") {
                auto guide_rate = telescope->get_guide_rate();
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, guide_rate.dec);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "guideraterightascension") {
                auto guide_rate = telescope->get_guide_rate();
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, guide_rate.ra);
                response.set_body(alpaca_response);
                return response;
            }
        }
        
        // GET/PUT properties
        if (method_name == "doesrefraction") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, false);
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                bool value = parse_bool("DoesRefraction");
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "slewsettletime") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, 0.0);
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("SlewSettleTime");
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "utcdate") {
            if (request.method() == HttpMethod::GET) {
                auto utc = telescope->get_utc_date();
                auto time_t = std::chrono::system_clock::to_time_t(utc);
                std::ostringstream oss;
                oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    utc.time_since_epoch()) % 1000;
                oss << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, oss.str());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                std::string date_str;
                bool found = false;
                
                // Try query parameter first
                if (request.has_query_param("UTCDate")) {
                    date_str = request.get_query_param("UTCDate");
                    found = true;
                } else if (!request.body().empty()) {
                    // Try JSON first
                    auto json_opt = parse_json(request.body());
                    if (json_opt) {
                        if (json_opt->contains("UTCDate")) {
                            date_str = json_opt->at("UTCDate").get<std::string>();
                            found = true;
                        } else if (json_opt->contains("Value")) {
                            date_str = json_opt->at("Value").get<std::string>();
                            found = true;
                        }
                    }
                    
                    // If JSON parsing failed or didn't contain UTCDate/Value, try form-encoded
                    if (!found) {
                        std::istringstream iss(request.body());
                        std::string pair;
                        while (std::getline(iss, pair, '&')) {
                            auto eq_pos = pair.find('=');
                            if (eq_pos != std::string::npos) {
                                std::string key = pair.substr(0, eq_pos);
                                std::string value = pair.substr(eq_pos + 1);
                                
                                if (key == "UTCDate" || key == "Value") {
                                    date_str = url_decode(value);
                                    found = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                
                if (!found) {
                    throw std::runtime_error("Missing parameter: UTCDate or Value");
                }
                std::tm tm = {};
                std::istringstream ss(date_str);
                ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
                if (ss.fail()) {
                    throw std::runtime_error("Invalid UTC date format: " + date_str);
                }
                auto time_point = std::chrono::system_clock::from_time_t(std::mktime(&tm));
                telescope->set_utc_date(time_point);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "tracking") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_tracking());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                bool value = parse_bool("Tracking");
                telescope->set_tracking(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "declinationrate") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_declination_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("DeclinationRate");
                telescope->set_declination_rate(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "rightascensionrate") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_right_ascension_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("RightAscensionRate");
                telescope->set_right_ascension_rate(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "trackingrate") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_tracking_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("TrackingRate");
                telescope->set_tracking_rate(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "targetdeclination") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_target_declination());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("TargetDeclination");
                telescope->set_target_declination(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "targetrightascension") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_target_right_ascension());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("TargetRightAscension");
                telescope->set_target_right_ascension(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "siteelevation") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_site_elevation());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("SiteElevation");
                telescope->set_site_elevation(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "sitelatitude") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_site_latitude());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("SiteLatitude");
                telescope->set_site_latitude(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "sitelongitude") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_site_longitude());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("SiteLongitude");
                telescope->set_site_longitude(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "sideofpier") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_side_of_pier());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                int value = parse_int("SideOfPier");
                telescope->set_side_of_pier(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "guideratedeclination") {
            if (request.method() == HttpMethod::GET) {
                auto guide_rate = telescope->get_guide_rate();
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, guide_rate.dec);
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("GuideRateDeclination");
                auto guide_rate = telescope->get_guide_rate();
                guide_rate.dec = value;
                telescope->set_guide_rate(guide_rate);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "guideraterightascension") {
            if (request.method() == HttpMethod::GET) {
                auto guide_rate = telescope->get_guide_rate();
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, guide_rate.ra);
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("GuideRateRightAscension");
                auto guide_rate = telescope->get_guide_rate();
                guide_rate.ra = value;
                telescope->set_guide_rate(guide_rate);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        
        // PUT-only methods (actions)
        if (request.method() == HttpMethod::PUT) {
            if (method_name == "abortslew") {
                telescope->abort_slew();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "moveaxis") {
                // Debug logging
                if (!request.body().empty()) {
                    util::log_info("moveaxis body: " + request.body());
                }
                int axis = parse_int("Axis");
                double rate = parse_double("Rate");
                util::log_info("moveaxis parsed: Axis=" + std::to_string(axis) + ", Rate=" + std::to_string(rate));
                telescope->move_axis(axis, rate);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewtoaltazasync") {
                double altitude = parse_double("Altitude");
                double azimuth = parse_double("Azimuth");
                telescope->slew_to_alt_az_async(altitude, azimuth);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "synctoaltaz") {
                double altitude = parse_double("Altitude");
                double azimuth = parse_double("Azimuth");
                telescope->sync_to_alt_az(altitude, azimuth);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "park") {
                telescope->park();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "unpark") {
                telescope->unpark();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "findhome") {
                telescope->find_home();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "setpark") {
                telescope->set_park();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewtotarget") {
                telescope->slew_to_target();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewtotargetasync") {
                telescope->slew_to_target_async();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewtocoordinates") {
                double ra = parse_double("RightAscension");
                double dec = parse_double("Declination");
                telescope->set_target_right_ascension(ra);
                telescope->set_target_declination(dec);
                telescope->slew_to_coordinates();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewtocoordinatesasync") {
                // Debug logging
                if (!request.body().empty()) {
                    util::log_info("slewtocoordinatesasync body: " + request.body());
                }
                double ra = parse_double("RightAscension");
                double dec = parse_double("Declination");
                util::log_info("slewtocoordinatesasync parsed: RA=" + std::to_string(ra) + ", Dec=" + std::to_string(dec));
                telescope->set_target_right_ascension(ra);
                telescope->set_target_declination(dec);
                telescope->slew_to_coordinates_async();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "synctotarget") {
                telescope->sync_to_target();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "synctocoordinates") {
                double ra = parse_double("RightAscension");
                double dec = parse_double("Declination");
                telescope->sync_to_coordinates(ra, dec);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "pulseguide") {
                int direction = parse_int("Direction");
                int duration = parse_int("Duration");
                telescope->pulse_guide(direction, duration);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        
        // Method not found
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for Telescope"
        );
        response.set_body(alpaca_response);
        return response;
        
    } catch (const alpacacore::AlpacaException& e) {
        util::log_error("AlpacaException in telescope method '" + method_name + "': " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::map_error_code(e.error_code()),
            std::string(e.what())
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in telescope method '" + method_name + "': " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::handle_root(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        try {
            client_tx_id = static_cast<std::uint32_t>(std::stoul(request.get_query_param("ClientTransactionID")));
        } catch (...) {
            // Invalid transaction ID
        }
    }

    // Return a simple JSON response with server information
    nlohmann::json info;
    info["ServerName"] = "AlpacaHTTP";
    info["Version"] = "0.1.0";
    info["ManagementAPI"] = "/management/v1";
    info["DeviceAPI"] = "/api/v1";
    info["Endpoints"] = nlohmann::json::array({
        "/management/v1/description",
        "/management/v1/configureddevices",
        "/api/v1/{devicetype}/{devicenumber}/{method}"
    });

    AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
    alpaca_response.value = info.dump();
    response.set_body(alpaca_response);
    return response;
}

Response Router::handle_api_versions(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        try {
            client_tx_id = static_cast<std::uint32_t>(std::stoul(request.get_query_param("ClientTransactionID")));
        } catch (...) {
            // Invalid transaction ID
        }
    }

    try {
        // Alpaca API versions endpoint returns array of supported API versions
        // Currently only version 1 is defined
        nlohmann::json versions = nlohmann::json::array({1});

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = versions.dump();
        response.set_body(alpaca_response);
        
        util::log_info("API versions response: " + versions.dump());
    } catch (const std::exception& e) {
        util::log_error("Error getting API versions: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
        response.set_body(alpaca_response);
    }

    return response;
}

Response Router::handle_static_file(const Request& request) {
    Response response;
    std::string file_path = request.path();
    
    // Map root to index.html
    if (file_path == "/" || file_path == "") {
        file_path = "/web/index.html";
    }
    
    // Remove leading slash and build full path
    if (file_path[0] == '/') {
        file_path = file_path.substr(1);
    }
    
    // Security: Only allow files from web directory
    if (file_path.find("web/") != 0 && file_path != "web/index.html") {
        response.set_status(403, "Forbidden");
        response.set_body("Access denied");
        return response;
    }
    
    // Extract filename from path (e.g., web/index.html -> index.html)
    std::string filename = file_path;
    size_t web_pos = file_path.find("web/");
    if (web_pos != std::string::npos) {
        filename = file_path.substr(web_pos + 4); // Skip "web/" (4 characters)
    }
    
    // Try multiple possible locations for web files
    std::vector<std::string> possible_paths = {
        "web/" + filename,                    // Current directory
        "../web/" + filename,                 // Parent directory (if running from build/)
        "../../web/" + filename,              // Two levels up
        "../AlpacaHTTP/web/" + filename,     // From build directory
        "AlpacaHTTP/web/" + filename         // Alternative location
    };
    
    std::ifstream file;
    std::string full_path;
    bool found = false;
    
    for (const auto& path : possible_paths) {
        file.open(path, std::ios::binary);
        if (file.is_open()) {
            full_path = path;
            found = true;
            break;
        }
        file.close();
    }
    
    if (!found) {
        response.set_status(404, "Not Found");
        std::string error_msg = "File not found: " + file_path + "\n";
        error_msg += "Searched in:\n";
        for (const auto& path : possible_paths) {
            error_msg += "  - " + path + "\n";
        }
        response.set_body(error_msg);
        return response;
    }
    
    // Read file content
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    
    // Set appropriate content type
    if (file_path.find(".html") != std::string::npos) {
        response.set_content_type("text/html");
    } else if (file_path.find(".css") != std::string::npos) {
        response.set_content_type("text/css");
    } else if (file_path.find(".js") != std::string::npos) {
        response.set_content_type("application/javascript");
    } else if (file_path.find(".json") != std::string::npos) {
        response.set_content_type("application/json");
    } else {
        response.set_content_type("text/plain");
    }
    
    response.set_body(content);
    return response;
}

Response Router::handle_setup(const Request& request, std::uint32_t server_tx_id) {
    Response response;

    util::log_info("Handling setup endpoint: " + request.path());

    // Setup endpoints are expected to return an HTML page.
    // We provide a simple stub page that points users to the web UI.
    // Example path: /setup/v1/telescope/0/setup
    std::regex setup_regex(R"(/setup/v1/([^/]+)/(\d+)/setup)");
    std::smatch matches;

    if (!std::regex_match(request.path(), matches, setup_regex)) {
        util::log_warning("Setup endpoint regex did not match: " + request.path());
        // Not a valid setup path; return 404 as Alpaca error.
        response.set_status(404, "Not Found");
        AlpacaResponse alpaca_response = make_error_response(
            0, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Endpoint not found: " + request.path()
        );
        response.set_body(alpaca_response);
        return response;
    }

    util::log_info("Setup endpoint matched for device type=" + matches[1].str() +
                   " device=" + matches[2].str());

    response.set_status(200, "OK");
    response.set_content_type("text/html");
    response.set_body(
        "<!DOCTYPE html><html><head><title>Alpaca Device Setup</title></head>"
        "<body><h2>Alpaca Device Setup</h2>"
        "<p>This device does not expose a custom setup page. "
        "Please use the AlpacaHTTP web interface at <a href=\"/\">/</a> to configure devices.</p>"
        "</body></html>"
    );
    return response;
}

Response Router::handle_configure_device(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        try {
            client_tx_id = static_cast<std::uint32_t>(std::stoul(request.get_query_param("ClientTransactionID")));
        } catch (...) {
            // Invalid transaction ID
        }
    }
    
    // Only allow POST or PUT requests
    if (request.method() != HttpMethod::POST && request.method() != HttpMethod::PUT) {
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Method not allowed. Use POST or PUT."
        );
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
        return response;
    }
    
    try {
        load_persisted_devices();

        // Parse JSON body
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(request.body());
        } catch (const nlohmann::json::exception& e) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Invalid JSON: " + std::string(e.what())
            );
            response.set_body(alpaca_response);
            return response;
        }
        
        std::string vendor = config.value("vendor", "");
        std::string device_type_str = config.value("deviceType", "");

        if (vendor.empty() || device_type_str.empty()) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Missing required fields: deviceType and vendor"
            );
            response.set_body(alpaca_response);
            return response;
        }

        std::string error_message;
        if (!register_device_from_config(config, error_message)) {
            if (error_message.empty()) {
                error_message = "Failed to register device. Please verify the configuration.";
            }
            std::int32_t error_code = util::ErrorCode::INVALID_VALUE;
            if (error_message.find("not yet supported") != std::string::npos ||
                error_message.find("not enabled") != std::string::npos) {
                error_code = util::ErrorCode::NOT_IMPLEMENTED;
            }
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                error_code,
                error_message
            );
            response.set_body(alpaca_response);
            return response;
        }

        add_or_replace_persisted_device(sanitize_device_config(config));
        save_persisted_devices();

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = std::string("Device registered successfully");
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Error configuring device: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::handle_remove_device(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        try {
            client_tx_id = static_cast<std::uint32_t>(std::stoul(request.get_query_param("ClientTransactionID")));
        } catch (...) {
            // Invalid transaction ID
        }
    }
    
    // Only allow POST or PUT requests
    if (request.method() != HttpMethod::POST && request.method() != HttpMethod::PUT) {
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Method not allowed. Use POST or PUT."
        );
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
        return response;
    }
    
    try {
        load_persisted_devices();

        // Parse JSON body
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(request.body());
        } catch (const nlohmann::json::exception& e) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Invalid JSON: " + std::string(e.what())
            );
            response.set_body(alpaca_response);
            return response;
        }
        
        std::string device_type_str = config.value("deviceType", "");
        std::string vendor = config.value("vendor", "");
        int device_number = config.value("deviceNumber", -1);
        
        if (device_type_str.empty() || device_number < 0) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Missing required fields: deviceType and deviceNumber"
            );
            response.set_body(alpaca_response);
            return response;
        }
        
        // Convert device type string to enum
        alpacacore::DeviceType device_type;
        try {
            device_type = string_to_device_type(device_type_str);
        } catch (const std::exception& e) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Invalid device type: " + device_type_str
            );
            response.set_body(alpaca_response);
            return response;
        }
        
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        
        if (registry.unregister_device(device_type, device_number)) {
            AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
            alpaca_response.value = std::string("Device removed successfully");
            response.set_body(alpaca_response);
            util::log_info("Removed device: " + device_type_str + " #" + std::to_string(device_number));
            remove_persisted_device(vendor, device_type_str, device_number);
            save_persisted_devices();
            return response;
        } else {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Device not found: " + device_type_str + " #" + std::to_string(device_number)
            );
            response.set_body(alpaca_response);
            return response;
        }
        
    } catch (const std::exception& e) {
        util::log_error("Error removing device: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::handle_log_level(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        try {
            client_tx_id = static_cast<std::uint32_t>(std::stoul(request.get_query_param("ClientTransactionID")));
        } catch (...) {
            // ignore invalid id
        }
    }

    auto send_payload = [&](std::uint32_t ctx_id) {
        AlpacaResponse alpaca_response(ctx_id, server_tx_id);
        alpaca_response.value = make_log_level_payload().dump();
        response.set_body(alpaca_response);
        return response;
    };

    try {
        if (request.method() == HttpMethod::GET) {
            return send_payload(client_tx_id);
        }

        if (request.method() == HttpMethod::POST || request.method() == HttpMethod::PUT) {
            if (request.body().empty()) {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::VALUE_NOT_SET,
                    "Missing request body"
                );
                response.set_body(err);
                return response;
            }

            auto json_opt = parse_json(request.body());
            if (!json_opt) {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::INVALID_VALUE,
                    "Invalid JSON payload"
                );
                response.set_body(err);
                return response;
            }

            const auto& body = *json_opt;
            auto body_client_tx = extract_client_transaction_id(body);
            if (body_client_tx != 0) {
                client_tx_id = body_client_tx;
            }

            std::string requested_level;
            if (body.contains("level")) {
                requested_level = body["level"].get<std::string>();
            } else if (body.contains("Level")) {
                requested_level = body["Level"].get<std::string>();
            } else {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::VALUE_NOT_SET,
                    "Request must include a 'level' property"
                );
                response.set_body(err);
                return response;
            }

            auto parsed_level = parse_log_level_string(requested_level);
            if (!parsed_level.has_value()) {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::INVALID_VALUE,
                    "Unknown log level: " + requested_level
                );
                response.set_body(err);
                return response;
            }

            alpacacore::logging::set_log_level(*parsed_level);
            util::log_info("Log level set to " + log_level_to_string(*parsed_level));
            return send_payload(client_tx_id);
        }

        AlpacaResponse err = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_OPERATION,
            "Unsupported HTTP method for log level endpoint"
        );
        response.set_body(err);
        return response;
    } catch (const std::exception& e) {
        AlpacaResponse err = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::DRIVER_ERROR,
            "Failed to update log level: " + std::string(e.what())
        );
        response.set_body(err);
        return response;
    }
}
Response Router::handle_shutdown(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        try {
            client_tx_id = static_cast<std::uint32_t>(std::stoul(request.get_query_param("ClientTransactionID")));
        } catch (...) {
            // Invalid transaction ID
        }
    }
    
    // Only allow POST or PUT requests
    if (request.method() != HttpMethod::POST && request.method() != HttpMethod::PUT) {
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Method not allowed. Use POST or PUT."
        );
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
        return response;
    }
    
    // Call shutdown callback if set
    if (shutdown_callback_) {
        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = std::string("Shutdown initiated");
        response.set_body(alpaca_response);
        
        // Call callback asynchronously (after response is sent)
        // Use a small delay to ensure response is sent first
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (shutdown_callback_) {
                shutdown_callback_();
            }
        }).detach();
        
        return response;
    } else {
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Shutdown callback not configured"
        );
        response.set_body(alpaca_response);
        return response;
    }
}

bool Router::register_device_from_config(const nlohmann::json& config, std::string& error_message) {
    std::string device_type_str = config.value("deviceType", "");
    std::string vendor = config.value("vendor", "");
    int device_number = config.value("deviceNumber", -1);

    if (device_type_str.empty() || vendor.empty() || device_number < 0) {
        error_message = "Missing required fields: deviceType, vendor, and deviceNumber";
        return false;
    }

    auto& registry = alpacacore::management::DeviceRegistry::instance();

    if (vendor == "ioptron" && device_type_str == "telescope") {
#ifdef ALPACACORE_ENABLE_IOPTRON
        alpacacore::vendor::ioptron::ConnectionInfo conn_info;
        std::string conn_type = config.value("connectionType", "");

        if (conn_type == "serial") {
            conn_info.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
            conn_info.port_path = config.value("portPath", "");
            conn_info.baud_rate = config.value("baudRate", 9600);

            if (conn_info.port_path.empty()) {
                error_message = "Serial port path is required";
                return false;
            }
        } else if (conn_type == "network") {
            conn_info.type = alpacacore::vendor::ioptron::ConnectionType::Network;
            conn_info.host = config.value("host", "");
            conn_info.tcp_port = config.value("tcpPort", 4030);

            if (conn_info.host.empty()) {
                error_message = "Host IP address is required";
                return false;
            }
        } else {
            error_message = "Invalid connection type. Use 'serial' or 'network'";
            return false;
        }

        conn_info.response_timeout_ms = config.value("responseTimeoutMs", conn_info.response_timeout_ms);

        std::optional<double> site_latitude;
        std::optional<double> site_longitude;
        std::optional<double> site_elevation;
        std::optional<bool> sync_time_on_connect;

        if (config.contains("siteLatitude")) {
            site_latitude = config.value("siteLatitude", 0.0);
        }
        if (config.contains("siteLongitude")) {
            site_longitude = config.value("siteLongitude", 0.0);
        }
        if (config.contains("siteElevation")) {
            site_elevation = config.value("siteElevation", 0.0);
        }
        if (config.contains("syncTimeOnConnect")) {
            sync_time_on_connect = config.value("syncTimeOnConnect", false);
        }

        auto telescope = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(
            device_number, conn_info, site_latitude, site_longitude, site_elevation, sync_time_on_connect);

        if (double aperture = config.value("apertureDiameter", 0.0); aperture > 0.0) {
            telescope->set_aperture_diameter(aperture);
        }
        if (double focal = config.value("focalLength", 0.0); focal > 0.0) {
            telescope->set_focal_length(focal);
        }
        if (site_elevation.has_value()) {
            telescope->set_site_elevation(site_elevation.value());
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(telescope.release()))) {
            util::log_info("Registered iOptron telescope");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "iOptron support not enabled. Rebuild with -DALPACACORE_ENABLE_IOPTRON=ON";
        return false;
#endif
    }

    error_message = "Vendor/device type combination not yet supported: " + vendor + "/" + device_type_str;
    return false;
}

nlohmann::json Router::sanitize_device_config(const nlohmann::json& config) const {
    nlohmann::json sanitized = nlohmann::json::object();

    auto copy_if_present = [&](const char* key) {
        if (config.contains(key)) {
            sanitized[key] = config.at(key);
        }
    };

    copy_if_present("vendor");
    copy_if_present("deviceType");
    copy_if_present("deviceNumber");
    copy_if_present("connectionType");

    std::string connection_type = config.value("connectionType", "");
    if (connection_type == "serial") {
        copy_if_present("portPath");
        copy_if_present("baudRate");
    } else if (connection_type == "network") {
        copy_if_present("host");
        copy_if_present("tcpPort");
    }

    copy_if_present("responseTimeoutMs");
    copy_if_present("apertureDiameter");
    copy_if_present("focalLength");
    copy_if_present("siteLatitude");
    copy_if_present("siteLongitude");
    copy_if_present("siteElevation");
    copy_if_present("syncTimeOnConnect");

    return sanitized;
}

void Router::add_or_replace_persisted_device(const nlohmann::json& config) {
    if (!config.contains("deviceType") || !config.contains("vendor") || !config.contains("deviceNumber")) {
        return;
    }

    for (auto& existing : persisted_devices_) {
        if (existing.value("vendor", "") == config.value("vendor", "") &&
            existing.value("deviceType", "") == config.value("deviceType", "") &&
            existing.value("deviceNumber", -1) == config.value("deviceNumber", -1)) {
            existing = config;
            return;
        }
    }

    persisted_devices_.push_back(config);
}

void Router::remove_persisted_device(const std::string& vendor, const std::string& device_type, int device_number) {
    if (device_type.empty() || device_number < 0) {
        return;
    }

    persisted_devices_.erase(
        std::remove_if(
            persisted_devices_.begin(),
            persisted_devices_.end(),
            [&](const nlohmann::json& entry) {
                if (entry.value("deviceType", "") != device_type ||
                    entry.value("deviceNumber", -1) != device_number) {
                    return false;
                }
                if (vendor.empty()) {
                    return true;
                }
                return entry.value("vendor", "") == vendor;
            }),
        persisted_devices_.end()
    );
}

void Router::save_persisted_devices() const {
    try {
        if (kPersistedDevicesFile.has_parent_path()) {
            std::filesystem::create_directories(kPersistedDevicesFile.parent_path());
        }

        std::ofstream out(kPersistedDevicesFile);
        if (!out) {
            throw std::runtime_error("Unable to open " + kPersistedDevicesFile.string() + " for writing");
        }

        nlohmann::json payload = persisted_devices_;
        out << payload.dump(4);
        out.flush();
    } catch (const std::exception& e) {
        util::log_error("Failed to persist registered devices: " + std::string(e.what()));
    }
}

void Router::load_persisted_devices() {
    if (persisted_devices_loaded_) {
        return;
    }

    persisted_devices_loaded_ = true;

    try {
        if (!std::filesystem::exists(kPersistedDevicesFile)) {
            persisted_devices_.clear();
            return;
        }

        std::ifstream in(kPersistedDevicesFile);
        if (!in) {
            throw std::runtime_error("Unable to open " + kPersistedDevicesFile.string() + " for reading");
        }

        nlohmann::json payload;
        in >> payload;

        if (!payload.is_array()) {
            throw std::runtime_error("Persisted device file must contain a JSON array");
        }

        persisted_devices_.clear();
        for (const auto& entry : payload) {
            std::string error_message;
            if (register_device_from_config(entry, error_message)) {
                persisted_devices_.push_back(sanitize_device_config(entry));
            } else {
                util::log_warning("Skipping persisted device: " + error_message);
            }
        }
    } catch (const std::exception& e) {
        util::log_error("Failed to load registered devices: " + std::string(e.what()));
        persisted_devices_.clear();
    }
}

} // namespace alpacahttp

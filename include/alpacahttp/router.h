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

#pragma once

#include "request.h"
#include "response.h"
#include <alpacacore/device_registry.h>
#include <alpacacore/alpaca_defs.h>
#include <alpacacore/managementdriver.h>
#include <alpacacore/telescope_driver.h>
#include <string>
#include <memory>
#include <functional>

namespace alpacahttp {

struct RouteMatch {
    std::string device_type;
    std::uint32_t device_number = 0;
    std::string method_name;
    bool is_management = false;
    std::string management_endpoint;
};

class Router {
public:
    Router();
    ~Router();

    // Set management driver (from AlpacaCore)
    void set_management_driver(std::shared_ptr<alpacacore::ManagementDriver> mgmt_driver);

    // Set shutdown callback (called when shutdown endpoint is requested)
    void set_shutdown_callback(std::function<void()> callback);

    // Route request and generate response
    Response route(const Request& request, std::uint32_t server_transaction_id);

private:
    std::shared_ptr<alpacacore::ManagementDriver> management_driver_;
    std::function<void()> shutdown_callback_;

    // Parse route from path
    RouteMatch parse_route(const std::string& path);

    // Convert device type string to enum
    alpacacore::DeviceType string_to_device_type(const std::string& type_str) const;

    // Handle management endpoints
    Response handle_management(const Request& request, const RouteMatch& match, std::uint32_t server_tx_id);
    Response handle_root(const Request& request, std::uint32_t server_tx_id);
    Response handle_description(const Request& request, std::uint32_t server_tx_id);
    Response handle_api_versions(const Request& request, std::uint32_t server_tx_id);
    Response handle_configured_devices(const Request& request, std::uint32_t server_tx_id);
    Response handle_configure_device(const Request& request, std::uint32_t server_tx_id);
    Response handle_remove_device(const Request& request, std::uint32_t server_tx_id);
    Response handle_shutdown(const Request& request, std::uint32_t server_tx_id);
    Response handle_static_file(const Request& request);
    Response handle_setup(const Request& request, std::uint32_t server_tx_id);

    // Handle device endpoints
    Response handle_device(const Request& request, const RouteMatch& match, std::uint32_t server_tx_id);
    
    // Dispatch device method calls
    Response dispatch_device_method(
        std::shared_ptr<alpacacore::AlpacaDriver> device,
        const std::string& method_name,
        const Request& request,
        std::uint32_t client_tx_id,
        std::uint32_t server_tx_id
    );
    
    // Dispatch telescope-specific method calls
    Response dispatch_telescope_method(
        std::shared_ptr<alpacacore::TelescopeDriver> telescope,
        const std::string& method_name,
        const Request& request,
        std::uint32_t client_tx_id,
        std::uint32_t server_tx_id
    );
};

} // namespace alpacahttp


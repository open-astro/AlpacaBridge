// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

#pragma once

#include "request.h"
#include "response.h"
#include <string>
#include <functional>
#include <memory>

namespace alpacahttp {

// Forward declaration - AlpacaCore integration
// TODO: Replace with actual AlpacaCore interface
class DeviceManager;

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

    // Set device manager (from AlpacaCore)
    void set_device_manager(std::shared_ptr<DeviceManager> device_manager);

    // Route request and generate response
    Response route(const Request& request, std::uint32_t server_transaction_id);

private:
    std::shared_ptr<DeviceManager> device_manager_;

    // Parse route from path
    RouteMatch parse_route(const std::string& path);

    // Handle management endpoints
    Response handle_management(const Request& request, const RouteMatch& match, std::uint32_t server_tx_id);
    Response handle_description(const Request& request, std::uint32_t server_tx_id);
    Response handle_configured_devices(const Request& request, std::uint32_t server_tx_id);

    // Handle device endpoints
    Response handle_device(const Request& request, const RouteMatch& match, std::uint32_t server_tx_id);
};

} // namespace alpacahttp


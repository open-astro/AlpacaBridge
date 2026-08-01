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

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/covercalibrator_driver.h>
#include <alpacacore/device_registry.h>
#include <alpacacore/dome_driver.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/managementdriver.h>
#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/rotator_driver.h>
#include <alpacacore/safetymonitor_driver.h>
#include <alpacacore/switch_driver.h>
#include <alpacacore/telescope_driver.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "request.h"
#include "response.h"
#include "version.h"

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
    void set_server_info(std::string server_name,
                         std::string manufacturer,
                         std::string manufacturer_version,
                         std::string location);
    void set_config_path(std::string config_path);

    // Set shutdown callback (called when shutdown endpoint is requested)
    void set_shutdown_callback(std::function<void()> callback);
    // Set restart callback (called when restart endpoint is requested)
    void set_restart_callback(std::function<void()> callback);

    // Route request and generate response
    Response route(const Request& request, std::uint32_t server_transaction_id);

private:
    std::shared_ptr<alpacacore::ManagementDriver> management_driver_;
    std::function<void()> shutdown_callback_;
    std::function<void()> restart_callback_;

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
    Response handle_restart(const Request& request, std::uint32_t server_tx_id);
    Response handle_log_level(const Request& request, std::uint32_t server_tx_id);
    Response handle_logs(const Request& request, std::uint32_t server_tx_id);
    Response handle_log_files_list(const Request& request, std::uint32_t server_tx_id);
    Response handle_log_file_item(const Request& request,
                                  const std::string& filename,
                                  std::uint32_t server_tx_id);
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

    bool register_device_from_config(const nlohmann::json& config, std::string& error_message);
    nlohmann::json sanitize_device_config(const nlohmann::json& config) const;
    void add_or_replace_persisted_device(const nlohmann::json& config);
    bool remove_persisted_device(const std::string& vendor, const std::string& device_type, int device_number);
    void save_persisted_devices() const;
    void load_persisted_devices();

    nlohmann::json build_description_payload() const;
    
    // Dispatch telescope-specific method calls
    Response dispatch_telescope_method(
        std::shared_ptr<alpacacore::TelescopeDriver> telescope,
        const std::string& method_name,
        const Request& request,
        std::uint32_t client_tx_id,
        std::uint32_t server_tx_id
    );

    Response dispatch_camera_method(
        std::shared_ptr<alpacacore::CameraDriver> camera,
        const std::string& method_name,
        const Request& request,
        std::uint32_t client_tx_id,
        std::uint32_t server_tx_id
    );

    Response dispatch_switch_method(
        std::shared_ptr<alpacacore::SwitchDriver> sw,
        const std::string& method_name,
        const Request& request,
        std::uint32_t client_tx_id,
        std::uint32_t server_tx_id
    );

    Response dispatch_filterwheel_method(
        std::shared_ptr<alpacacore::FilterWheelDriver> filterwheel,
        const std::string& method_name,
        const Request& request,
        std::uint32_t client_tx_id,
        std::uint32_t server_tx_id
    );

    Response dispatch_focuser_method(
        std::shared_ptr<alpacacore::FocuserDriver> focuser,
        const std::string& method_name,
        const Request& request,
        std::uint32_t client_tx_id,
        std::uint32_t server_tx_id
    );

    Response dispatch_rotator_method(
        std::shared_ptr<alpacacore::RotatorDriver> rotator,
        const std::string& method_name,
        const Request& request,
        std::uint32_t client_tx_id,
        std::uint32_t server_tx_id
    );

    Response dispatch_dome_method(
        std::shared_ptr<alpacacore::DomeDriver> dome,
        const std::string& method_name,
        const Request& request,
        std::uint32_t client_tx_id,
        std::uint32_t server_tx_id
    );

    Response dispatch_covercalibrator_method(
        std::shared_ptr<alpacacore::CoverCalibratorDriver> covercalibrator,
        const std::string& method_name,
        const Request& request,
        std::uint32_t client_tx_id,
        std::uint32_t server_tx_id
    );

    Response dispatch_observingconditions_method(
        std::shared_ptr<alpacacore::ObservingConditionsDriver> observingconditions,
        const std::string& method_name,
        const Request& request,
        std::uint32_t client_tx_id,
        std::uint32_t server_tx_id
    );

    Response dispatch_safetymonitor_method(
        std::shared_ptr<alpacacore::SafetyMonitorDriver> safetymonitor,
        const std::string& method_name,
        const Request& request,
        std::uint32_t client_tx_id,
        std::uint32_t server_tx_id
    );

    // Per-client Connected tracking (issue #160). Alpaca allows several
    // clients to share one device (imaging app + guider on the same mount);
    // the upstream link must only be torn down when the LAST client
    // disconnects. Keyed by driver instance; the inner map records each
    // ClientID's last-seen time so registrations from vanished clients can
    // expire instead of pinning the device connected forever.
    std::size_t register_client_connection(const void* device, const std::string& client_key);
    std::size_t unregister_client_connection(const void* device, const std::string& client_key);
    bool client_connection_registered(const void* device, const std::string& client_key);
    void touch_client_connection(const void* device, const std::string& client_key);
    void clear_client_connections(const void* device);

    // Serializes the connect/disconnect DECISION + driver call per device so
    // a client connecting during another client's last-out teardown can't
    // register against a link that is about to drop (review of #160). Held
    // across the blocking connect/disconnect waits — same-device connection
    // ops queue; everything else (other devices, GETs) is unaffected.
    std::shared_ptr<std::mutex> device_connection_op_mutex(const void* device);

    // Guards client_connections_ only; never held across driver calls.
    mutable std::mutex client_connections_mutex_;
    std::unordered_map<const void*, std::unordered_map<std::string, std::chrono::steady_clock::time_point>>
        client_connections_;
    std::unordered_map<const void*, std::shared_ptr<std::mutex>> connection_op_mutexes_;

    // Guards persisted_devices_ and persisted_devices_loaded_. Never held
    // together with server_info_mutex_ or across driver-registry calls.
    mutable std::mutex persisted_devices_mutex_;
    std::vector<nlohmann::json> persisted_devices_;
    bool persisted_devices_loaded_ = false;

    mutable std::mutex server_info_mutex_;
    std::string server_name_ = "AlpacaHTTP";
    std::string manufacturer_ = "AlpacaHTTP";
    // Default version - will be overridden by set_server_info() which uses alpacahttp::kVersion
    // This fallback uses the version constant from version.h (which comes from VERSION file via CMake)
    std::string manufacturer_version_ = alpacahttp::kVersion;
    std::string location_;
    std::string config_path_;
};

} // namespace alpacahttp

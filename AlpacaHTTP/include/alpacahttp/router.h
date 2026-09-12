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
#include <alpacacore/util/host_clock.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "request.h"
#include "response.h"
#include "version.h"
#include "wifi_manager.h"

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
    void set_server_info(std::string server_name, std::string manufacturer, std::string manufacturer_version,
                         std::string location, std::string profile_name = "");
    void set_config_path(std::string config_path);
    // open-astro#289: whether a client's Telescope.UTCDate write may step the
    // host clock when the kernel reports it undisciplined (no NTP/RTC).
    void set_sync_system_clock_from_clients(bool enabled) { host_clock_->set_enabled(enabled); }
    bool sync_system_clock_from_clients() const { return host_clock_->enabled(); }

    // Test-only seam (open-astro#302): replace the host clock with one whose
    // three probes are fakes (adjtimex, clock_settime and the sysfs RTC
    // read), so the #289 wiring -- the UTCDate PUT stepping
    // the clock before the driver sees the value, and the connect-time
    // warning -- can be driven in a test without touching the real system
    // clock. NOT the synctime endpoint: handle_sync_time() calls
    // clock_settime() directly and only its mark_stepped()/mark_step_failed()
    // bookkeeping goes through this object, so a test must never POST it an
    // in-range epoch even with the hooks installed. The current
    // syncSystemClockFromClients setting carries over; the step latches
    // (stepped_, step_failed_) deliberately do not, since a replacement clock
    // starts from "nothing has happened to it yet" -- which is what a test
    // installing hooks before serving wants.
    //
    // Replaces the clock object rather than mutating it, so it must be called
    // before the router serves any request; no request path may be in flight.
    // Since open-astro#314 that means before Server::start(): the RTC probe
    // thread dereferences host_clock_ too, so the seam now has a second
    // reader that is not a request path.
    void set_host_clock_hooks(
        alpacacore::util::HostClock::IsSynchronizedFn is_synchronized, alpacacore::util::HostClock::SetTimeFn set_time,
        alpacacore::util::HostClock::HasRtcFn has_rtc = [] { return false; });

    // open-astro#314: re-run the hardware-RTC probe and cache the answer.
    // Called from the server's RTC probe thread, never from a request path
    // and never from the reactor: the
    // probe is an I2C transaction on a bus-attached RTC and can block for the
    // adapter timeout, and the connect initiator it used to sit on is timed
    // against the 1 s STANDARD target.
    void refresh_rtc_probe() { host_clock_->refresh_rtc(); }

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

    // Lazily constructed on first /management/v1/wifi/* request so setups
    // without NetworkManager (or without a wifi adapter) pay no cost.
    // Persists its state (regulatory country) under the same relative
    // "config" dir as registered_devices.json.
    std::unique_ptr<util::WifiManager> wifi_manager_;
    std::mutex wifi_manager_init_mutex_;
    util::WifiManager& wifi_manager();

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
    Response handle_sync_time(const Request& request, std::uint32_t server_tx_id);
    // WiFi manager (see docs/wifi-manager-design.md); match.method_name
    // carries the sub-endpoint (status/scan/profiles/connect/ap/country/radio)
    // and, for profile deletes, the UUID.
    Response handle_wifi(const Request& request, const RouteMatch& match, std::uint32_t server_tx_id);
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

    // open-astro#274: where a device config came from decides what a failed
    // validation rule means. A config arriving over
    // /management/v1/configuredevice can still be corrected by the caller, so
    // it is rejected. One already on disk cannot: dropping it removes the
    // device from the registry, and /management/v1/configureddevices -- the
    // web UI's only source of devices -- then cannot show it, leaving no way
    // to edit the entry that is at fault. Persisted configs are registered
    // anyway and left for the driver's connect-time guard to refuse.
    enum class ConfigSource : std::uint8_t { Api, Persisted };

    bool register_device_from_config(const nlohmann::json& config, std::string& error_message,
                                     ConfigSource source = ConfigSource::Api);

    // Issue #380, generalising the rule #353 introduced for the Sky-Watcher
    // site-coordinate check: what a validation failure inside
    // register_device_from_config() *means* depends on where the config came
    // from, and every branch was otherwise left to remember that on its own.
    //
    // From the API (/management/v1/configuredevice) a bad config is a bad
    // request: refuse it. Nothing has been persisted, and the caller sees why.
    //
    // From disk the config is already saved, and the only way an operator can
    // repair it is the web UI -- whose sole source of devices is
    // /management/v1/configureddevices, which lists the DeviceRegistry. A
    // persisted config the router refuses never enters that registry, so
    // rejecting it here makes the device vanish from the UI with no way to
    // edit the entry that is at fault; recovery means hand-editing
    // registered_devices.json on the SBC. So it is registered anyway with a
    // WARN, and the driver's connect fails with the real reason.
    //
    // Returns true when the caller must reject (API, error_message set), and
    // false when it should carry on with the value it has (persisted, warning
    // logged).
    static bool reject_invalid_config(ConfigSource source, const char* reason, const std::string& vendor,
                                      const std::string& device_type, int device_number, std::string& error_message);

    // The connection-type half of the same rule. Returns conn_type unchanged
    // when it is empty (which every branch reads as auto-detect), when it is
    // one of `valid`, or when the config came from the API -- there the
    // branch's own else still rejects it. For a persisted config with an
    // unrecognised value it warns and returns "serial", so the device
    // registers and stays editable and the connect then fails on the port
    // path rather than auto-probing and attaching to whatever mount answers.
    static std::string normalize_persisted_connection_type(ConfigSource source, const std::string& conn_type,
                                                           std::initializer_list<const char*> valid,
                                                           const std::string& vendor, const std::string& device_type,
                                                           int device_number);
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
    // Erase BOTH maps' entries for a removed device (issue #162). Never call
    // while the device's op mutex is held — see clear_client_connections.
    void purge_device_connection_state(const void* device);

    // Serializes the connect/disconnect DECISION + driver call per device so
    // a client connecting during another client's last-out teardown can't
    // register against a link that is about to drop (review of #160). Held
    // across the blocking connect/disconnect waits — same-device connection
    // ops queue; everything else (other devices, GETs) is unaffected.
    std::shared_ptr<std::mutex> device_connection_op_mutex(const std::shared_ptr<alpacacore::AlpacaDriver>& device);

    void add_clock_fields(nlohmann::json& desc) const;
    void warn_if_clock_undisciplined(alpacacore::AlpacaDriver& device) const;

    // True while `device` is still the DeviceRegistry's driver for its
    // type/number. Straggler requests that fetched the shared_ptr before a
    // removedevice must not re-insert registry/op-mutex entries for it —
    // nothing would ever reap them (PR #164 review of issue #162).
    static bool device_is_current(const std::shared_ptr<alpacacore::AlpacaDriver>& device);

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
    std::string profile_name_;
    std::string config_path_;

    // Thread-safe; owns the "has a client stepped the clock" state (#289).
    // By pointer only so set_host_clock_hooks() can swap in a fake before the
    // router starts serving (#302); HostClock holds a mutex and so is neither
    // copyable nor assignable.
    std::unique_ptr<alpacacore::util::HostClock> host_clock_ = std::make_unique<alpacacore::util::HostClock>();
};

} // namespace alpacahttp

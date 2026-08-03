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

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/alpacadriver.h>
#include <alpacacore/device_registry.h>
#include <alpacahttp/request.h>
#include <alpacahttp/router.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "test_assert.h"

namespace {

alpacahttp::Response route_request(alpacahttp::Router& router, const std::string& method, const std::string& path,
                                   const std::string& body = std::string(),
                                   const std::string& remote_addr = std::string()) {
    alpacahttp::Request request;
    request.set_remote_address(remote_addr);
    std::ostringstream raw;
    raw << method << " " << path << " HTTP/1.1\r\n";
    raw << "Host: localhost\r\n";
    if (!body.empty()) {
        raw << "Content-Type: application/json\r\n";
        raw << "Content-Length: " << body.size() << "\r\n";
    }
    raw << "\r\n";
    raw << body;

    EXPECT(request.parse(raw.str()));
    return router.route(request, 1);
}

// Issue #102 back-fill helper: POST a device config, then read it back from
// configureddevices. Returns the round-tripped Config object for
// (device_type, device_number), or a null json if configuration failed or the
// device is missing — callers EXPECT(!cfg.is_null()) first, then assert every
// persisted field survived (the automated catch for sanitize_device_config
// allowlist gaps and FormData-style key loss).
nlohmann::json roundtrip_config(alpacahttp::Router& router, const nlohmann::json& configure_body,
                                const std::string& device_type, int device_number) {
    const auto configure_response =
        route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
    // Non-JSON bodies (server error paths) yield a null return -- a clean
    // EXPECT diagnostic -- rather than an uncaught parse_error.
    const auto configure_json = nlohmann::json::parse(configure_response.body(), nullptr, false);
    if (configure_json.is_discarded() || configure_json.value("ErrorNumber", -1) != 0) {
        return nlohmann::json();
    }
    const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
    const auto configured_json = nlohmann::json::parse(configured_response.body(), nullptr, false);
    if (configured_json.is_discarded()) {
        return nlohmann::json();
    }
    // An error envelope has no "Value"; return null for a clean EXPECT
    // diagnostic instead of an uncaught json::out_of_range on the const
    // subscript below.
    if (!configured_json.contains("Value") || !configured_json["Value"].is_array()) {
        return nlohmann::json();
    }
    for (const auto& entry : configured_json["Value"]) {
        if (entry.value("DeviceType", "") == device_type && entry.value("DeviceNumber", -1) == device_number) {
            return entry.value("Config", nlohmann::json());
        }
    }
    return nlohmann::json();
}

void remove_device(alpacahttp::Router& router, const std::string& vendor, const std::string& device_type,
                   int device_number) {
    nlohmann::json body = {{"vendor", vendor}, {"deviceType", device_type}, {"deviceNumber", device_number}};
    const auto response = route_request(router, "POST", "/management/v1/removedevice", body.dump());
    const auto json = nlohmann::json::parse(response.body(), nullptr, false);
    // A failed cleanup leaves the device registered and poisons later blocks
    // that reuse the number or read configureddevices -- fail HERE instead.
    EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
}

// Minimal driver used to verify the management configureddevices response
// surfaces get_device_firmware() and get_device_sdk_version() (web-UI only)
// when, and only when, the driver reports each value.
class FirmwareStubDriver final : public alpacacore::AlpacaDriver {
public:
    FirmwareStubDriver(int number, std::optional<std::string> firmware,
                       std::optional<std::string> sdk_version = std::nullopt)
        : number_(number), firmware_(std::move(firmware)), sdk_version_(std::move(sdk_version)) {}

    int get_device_number() const override { return number_; }
    std::string get_name() const override { return "Firmware Stub"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::CoverCalibrator; }
    std::string get_unique_id() const override { return "firmware-stub-" + std::to_string(number_); }
    std::string get_description() const override { return "fake device"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 1; }
    bool get_connected() const override { return true; }
    void set_connected(bool) override {}
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return ""; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return ""; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return ""; }

    std::optional<std::string> get_device_firmware() const override { return firmware_; }
    std::optional<std::string> get_device_sdk_version() const override { return sdk_version_; }

private:
    int number_;
    std::optional<std::string> firmware_;
    std::optional<std::string> sdk_version_;
};

// Connectable stub for the per-client Connected refcounting tests
// (issue #160): tracks real connect/disconnect calls so the tests can assert
// the upstream link is only touched by the first client in / last client out.
class ConnectStubDriver final : public alpacacore::AlpacaDriver {
public:
    explicit ConnectStubDriver(int number) : number_(number) {}

    int get_device_number() const override { return number_; }
    std::string get_name() const override { return "Connect Stub"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::CoverCalibrator; }
    std::string get_unique_id() const override { return "connect-stub-" + std::to_string(number_); }
    std::string get_description() const override { return "fake device"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 1; }
    bool get_connected() const override { return connected_; }
    void set_connected(bool connected) override {
        if (connected && !connected_) {
            ++connect_count;
        } else if (!connected && connected_) {
            ++disconnect_count;
        }
        connected_ = connected;
    }
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return ""; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return ""; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return ""; }

    // Simulate the upstream link dying underneath the bridge (USB unplug,
    // serial wedge) without going through disconnect().
    void drop_link() { connected_ = false; }

    int connect_count = 0;
    int disconnect_count = 0;

private:
    int number_;
    bool connected_ = false;
};

// GET .../connected for a given ClientID (no ClientID when client_id is empty)
// and return the reported Value.
bool get_connected_value(alpacahttp::Router& router, const std::string& path_base, const std::string& client_id,
                         const std::string& remote_addr = std::string()) {
    std::string path = path_base + "/connected";
    if (!client_id.empty()) {
        path += "?ClientID=" + client_id;
    }
    const auto resp = route_request(router, "GET", path, "", remote_addr);
    const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
    EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
    return json.value("Value", false);
}

// PUT .../connected with a form body; expects success unless expect_error.
void put_connected(alpacahttp::Router& router, const std::string& path_base, const std::string& client_id,
                   bool connected, const std::string& remote_addr = std::string()) {
    std::string body = "Connected=" + std::string(connected ? "true" : "false");
    if (!client_id.empty()) {
        body += "&ClientID=" + client_id;
    }
    const auto resp = route_request(router, "PUT", path_base + "/connected", body, remote_addr);
    const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
    EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
}

} // namespace

int main() {
    std::cout << "Testing routing...\n";

    alpacahttp::Router router;
    alpacahttp::Request request;

    // Test management endpoint parsing
    std::string test_request = "GET /management/v1/description HTTP/1.1\r\n\r\n";
    EXPECT(request.parse(test_request));
    EXPECT(request.path() == "/management/v1/description");

    // Test device endpoint parsing
    test_request = "GET /api/v1/camera/0/canconnect HTTP/1.1\r\n\r\n";
    EXPECT(request.parse(test_request));
    EXPECT(request.path() == "/api/v1/camera/0/canconnect");

    // Test query parameters
    test_request = "GET /api/v1/mount/0/slewto?RightAscension=1.5&Declination=-20.3 HTTP/1.1\r\n\r\n";
    EXPECT(request.parse(test_request));
    EXPECT(request.path() == "/api/v1/mount/0/slewto");
    EXPECT(request.has_query_param("RightAscension"));
    EXPECT(request.has_query_param("Declination"));

    // Conformance: "Parameter names are not case sensitive, so clients and
    // drivers should be prepared for parameter names to be supplied ... with
    // any casing." Query parameter lookups must match regardless of casing.
    EXPECT(request.has_query_param("rightascension"));
    EXPECT(request.has_query_param("DECLINATION"));
    EXPECT(request.get_query_param("RIGHTASCENSION") == "1.5");
    EXPECT(request.get_query_param("declination") == "-20.3");
    EXPECT(!request.has_query_param("nonexistent"));

    // Conformance: HTTP 400 "indicates that the device could not interpret the
    // request e.g. an invalid device number or misspelt device type". These
    // must be 400 (Bad Request), not 404, and carry a non-zero ErrorNumber.
    {
        // Misspelt / unknown device type.
        const auto resp = route_request(router, "GET", "/api/v1/wibble/0/connected");
        EXPECT(resp.status_code() == 400);
        const auto json = nlohmann::json::parse(resp.body());
        EXPECT(json.value("ErrorNumber", 0) != 0);
    }
    {
        // Valid device type, unknown method.
        const auto resp = route_request(router, "GET", "/api/v1/telescope/0/notarealmethod");
        EXPECT(resp.status_code() == 400);
        const auto json = nlohmann::json::parse(resp.body());
        EXPECT(json.value("ErrorNumber", 0) != 0);
    }
    {
        // Valid type and method, but no device registered at that number.
        const auto resp = route_request(router, "GET", "/api/v1/telescope/4242/connected");
        EXPECT(resp.status_code() == 400);
        const auto json = nlohmann::json::parse(resp.body());
        EXPECT(json.value("ErrorNumber", 0) != 0);
    }

#ifdef ALPACACORE_ENABLE_ZWO
    // Ensure idempotent behavior across repeated test runs.
    {
        nlohmann::json remove_body = {
            {"vendor", "zwo"},
            {"deviceType", "telescope"},
            {"deviceNumber", 9101}
        };
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        nlohmann::json configure_body = {
            {"vendor", "zwo"},
            {"deviceType", "telescope"},
            {"deviceNumber", 9101},
            {"connectionType", "serial"},
            {"portPath", "/dev/null"},
            {"baudRate", 9600},
            {"responseTimeoutMs", 2500},
            {"apertureDiameter", 0.1},
            {"focalLength", 0.8},
            {"siteLatitude", 34.5},
            {"siteLongitude", -117.2},
            {"siteElevation", 450.0},
            {"syncTimeOnConnect", false}
        };

        const auto configure_response = route_request(
            router,
            "POST",
            "/management/v1/configuredevice",
            configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_ZWO
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_device = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Telescope" &&
                entry.value("DeviceNumber", -1) == 9101) {
                EXPECT(entry.value("Vendor", "") == "zwo");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "zwo");
                EXPECT(cfg.value("deviceType", "") == "telescope");
                EXPECT(cfg.value("connectionType", "") == "serial");
                EXPECT(cfg.value("portPath", "") == "/dev/null");
                EXPECT(cfg.value("responseTimeoutMs", -1) == 2500);
                EXPECT(std::abs(cfg.value("apertureDiameter", 0.0) - 0.1) < 1e-12);
                EXPECT(std::abs(cfg.value("focalLength", 0.0) - 0.8) < 1e-12);
                EXPECT(std::abs(cfg.value("siteLatitude", 0.0) - 34.5) < 1e-12);
                EXPECT(std::abs(cfg.value("siteLongitude", 0.0) - (-117.2)) < 1e-12);
                EXPECT(std::abs(cfg.value("siteElevation", 0.0) - 450.0) < 1e-12);
                EXPECT(cfg.value("syncTimeOnConnect", true) == false);
                found_device = true;
                break;
            }
        }
        EXPECT(found_device);

        // Regression: device-API array responses (SupportedActions, DeviceState,
        // AxisRates, Gains, ...) must serialize as a JSON array, not a string.
        // The structured-Value change in to_json briefly left these handlers
        // passing a ".dump()"-ed string to make_success_response, which ConformU
        // rejected ("The JSON value could not be converted to IList<String>").
        const auto actions_response = route_request(router, "GET", "/api/v1/telescope/9101/supportedactions");
        const auto actions_json = nlohmann::json::parse(actions_response.body());
        EXPECT(actions_json.value("ErrorNumber", -1) == 0);
        EXPECT(actions_json.contains("Value"));
        EXPECT(actions_json["Value"].is_array());

        nlohmann::json remove_body = {
            {"vendor", "zwo"},
            {"deviceType", "telescope"},
            {"deviceNumber", 9101}
        };
        const auto remove_response = route_request(
            router,
            "POST",
            "/management/v1/removedevice",
            remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- Regression: a malformed "ports" array must not crash the router ---
    // A non-object ports entry (null / string / number) previously made the
    // libgpiod switch registration call nlohmann contains()/value() on a
    // non-object, throwing type_error (an uncaught 500). The router now skips
    // non-object entries; registration must complete with a clean response.
#if defined(ALPACACORE_ENABLE_TOUPTEK) && defined(ALPACACORE_TOUPTEK_STELLAVITA)
    {
        nlohmann::json ports = nlohmann::json::array();
        ports.push_back(nullptr);
        ports.push_back("foo");
        ports.push_back(42);
        ports.push_back({{"pwm", true}});  // only this valid entry is applied
        nlohmann::json configure_body = {
            {"vendor", "touptek"}, {"deviceType", "switch"}, {"deviceNumber", 9171}, {"ports", ports}};
        // Must return a well-formed response (no uncaught type_error → 500);
        // the malformed entries are skipped and registration succeeds.
        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        nlohmann::json remove_body = {{"vendor", "touptek"}, {"deviceType", "switch"}, {"deviceNumber", 9171}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
    }
#endif

    // --- ToupTek thermal switch routing test ---
    // The (touptek, switch) route selects between the StellaVita PowerBox and
    // the cooled-camera thermal switch (dew heater / fan / tail LED) via
    // switchType. The "thermal" backend is available on any ToupTek build (it
    // needs no libgpiod), registers without hardware, and must round-trip its
    // switchType discriminator and cameraIndex binding.
#ifdef ALPACACORE_ENABLE_TOUPTEK
    {
        nlohmann::json configure_body = {{"vendor", "touptek"},
                                         {"deviceType", "switch"},
                                         {"deviceNumber", 9181},
                                         {"switchType", "thermal"},
                                         {"cameraIndex", 2}};
        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        bool found_touptek_thermal = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Switch" && entry.value("DeviceNumber", -1) == 9181) {
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "touptek");
                EXPECT(cfg.value("switchType", "") == "thermal");
                EXPECT(cfg.value("cameraIndex", -1) == 2);
                found_touptek_thermal = true;
                break;
            }
        }
        EXPECT(found_touptek_thermal);

        nlohmann::json remove_body = {{"vendor", "touptek"}, {"deviceType", "switch"}, {"deviceNumber", 9181}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        EXPECT(nlohmann::json::parse(remove_response.body()).value("ErrorNumber", -1) == 0);
    }
    {
        // An unknown/typo'd switchType (e.g. wrong case "Thermal") must be
        // rejected, never silently fall through to a StellaVita registration.
        nlohmann::json configure_body = {
            {"vendor", "touptek"}, {"deviceType", "switch"}, {"deviceNumber", 9182}, {"switchType", "Thermal"}};
        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
    }
#endif

    // --- Player One thermal switch routing test ---
#ifdef ALPACACORE_ENABLE_PLAYERONE
    {
        // Runtime heater/fan control is switch-only by design: no connect-time
        // heater/fan camera config exists (a persisted "heater on" would
        // silently re-apply months later), so there is nothing camera-side to
        // round-trip beyond cameraIndex.
        nlohmann::json configure_body = {
            {"vendor", "playerone"}, {"deviceType", "camera"}, {"deviceNumber", 9210}, {"cameraIndex", 0}};
        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        nlohmann::json remove_body = {{"vendor", "playerone"}, {"deviceType", "camera"}, {"deviceNumber", 9210}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        EXPECT(nlohmann::json::parse(remove_response.body()).value("ErrorNumber", -1) == 0);
    }
    {
        // Thermal switch (dew heater / fan) registers without hardware and
        // persists its camera index.
        nlohmann::json configure_body = {
            {"vendor", "playerone"}, {"deviceType", "switch"}, {"deviceNumber", 9211}, {"cameraIndex", 1}};
        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        bool found_playerone_switch = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Switch" && entry.value("DeviceNumber", -1) == 9211) {
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "playerone");
                EXPECT(cfg.value("cameraIndex", -1) == 1);
                found_playerone_switch = true;
                break;
            }
        }
        EXPECT(found_playerone_switch);

        nlohmann::json remove_body = {{"vendor", "playerone"}, {"deviceType", "switch"}, {"deviceNumber", 9211}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        EXPECT(nlohmann::json::parse(remove_response.body()).value("ErrorNumber", -1) == 0);
    }
#endif

    // --- Celestron telescope routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_CELESTRON
    {
        nlohmann::json remove_body = {
            {"vendor", "celestron"},
            {"deviceType", "telescope"},
            {"deviceNumber", 9102}
        };
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        nlohmann::json configure_body = {
            {"vendor", "celestron"},
            {"deviceType", "telescope"},
            {"deviceNumber", 9102},
            {"connectionType", "serial"},
            {"portPath", "/dev/null"},
            {"baudRate", 9600},
            {"responseTimeoutMs", 5000},
            {"apertureDiameter", 0.28},
            {"focalLength", 2.8},
            {"siteLatitude", 33.85},
            {"siteLongitude", -118.34},
            {"siteElevation", 100.0},
            {"syncTimeOnConnect", true}
        };

        const auto configure_response = route_request(
            router,
            "POST",
            "/management/v1/configuredevice",
            configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_CELESTRON
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_celestron = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Telescope" &&
                entry.value("DeviceNumber", -1) == 9102) {
                EXPECT(entry.value("Vendor", "") == "celestron");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "celestron");
                EXPECT(cfg.value("deviceType", "") == "telescope");
                EXPECT(cfg.value("connectionType", "") == "serial");
                EXPECT(cfg.value("portPath", "") == "/dev/null");
                EXPECT(cfg.value("baudRate", -1) == 9600);
                EXPECT(cfg.value("responseTimeoutMs", -1) == 5000);
                EXPECT(std::abs(cfg.value("apertureDiameter", 0.0) - 0.28) < 1e-12);
                EXPECT(std::abs(cfg.value("focalLength", 0.0) - 2.8) < 1e-12);
                EXPECT(std::abs(cfg.value("siteLatitude", 0.0) - 33.85) < 1e-12);
                EXPECT(std::abs(cfg.value("siteLongitude", 0.0) - (-118.34)) < 1e-12);
                EXPECT(std::abs(cfg.value("siteElevation", 0.0) - 100.0) < 1e-12);
                EXPECT(cfg.value("syncTimeOnConnect", false) == true);
                found_celestron = true;
                break;
            }
        }
        EXPECT(found_celestron);

        // Test network connection type sanitization
        nlohmann::json net_configure_body = {
            {"vendor", "celestron"},
            {"deviceType", "telescope"},
            {"deviceNumber", 9103},
            {"connectionType", "network"},
            {"host", "192.168.1.100"},
            {"tcpPort", 2000}
        };

        const auto net_response = route_request(
            router,
            "POST",
            "/management/v1/configuredevice",
            net_configure_body.dump());
        const auto net_json = nlohmann::json::parse(net_response.body());
        EXPECT(net_json.value("ErrorNumber", -1) == 0);

        const auto net_configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto net_configured_json = nlohmann::json::parse(net_configured_response.body());
        bool found_net_celestron = false;
        for (const auto& entry : net_configured_json["Value"]) {
            if (entry.value("DeviceNumber", -1) == 9103) {
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("connectionType", "") == "network");
                EXPECT(cfg.value("host", "") == "192.168.1.100");
                EXPECT(cfg.value("tcpPort", -1) == 2000);
                found_net_celestron = true;
                break;
            }
        }
        EXPECT(found_net_celestron);

        // Cleanup
        nlohmann::json remove_body = {
            {"vendor", "celestron"},
            {"deviceType", "telescope"},
            {"deviceNumber", 9102}
        };
        const auto remove_response = route_request(
            router,
            "POST",
            "/management/v1/removedevice",
            remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);

        nlohmann::json remove_net_body = {
            {"vendor", "celestron"},
            {"deviceType", "telescope"},
            {"deviceNumber", 9103}
        };
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_net_body.dump());
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- ToupTek camera routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_TOUPTEK
    {
        nlohmann::json remove_body = {
            {"vendor", "touptek"},
            {"deviceType", "camera"},
            {"deviceNumber", 9201}
        };
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        nlohmann::json configure_body = {
            {"vendor", "touptek"},
            {"deviceType", "camera"},
            {"deviceNumber", 9201},
            {"cameraIndex", 2}
        };

        const auto configure_response = route_request(
            router,
            "POST",
            "/management/v1/configuredevice",
            configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_TOUPTEK
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_touptek = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Camera" &&
                entry.value("DeviceNumber", -1) == 9201) {
                EXPECT(entry.value("Vendor", "") == "touptek");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "touptek");
                EXPECT(cfg.value("deviceType", "") == "camera");
                EXPECT(cfg.value("cameraIndex", -1) == 2);
                found_touptek = true;
                break;
            }
        }
        EXPECT(found_touptek);

        nlohmann::json remove_body = {
            {"vendor", "touptek"},
            {"deviceType", "camera"},
            {"deviceNumber", 9201}
        };
        const auto remove_response = route_request(
            router,
            "POST",
            "/management/v1/removedevice",
            remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- ToupTek AAF focuser routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_TOUPTEK
    {
        nlohmann::json remove_body = {
            {"vendor", "touptek"},
            {"deviceType", "focuser"},
            {"deviceNumber", 9202}
        };
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        nlohmann::json configure_body = {
            {"vendor", "touptek"},
            {"deviceType", "focuser"},
            {"deviceNumber", 9202},
            {"focuserIndex", 0},
            {"focuserId", "tp-aaf-routing-test"}
        };

        const auto configure_response = route_request(
            router,
            "POST",
            "/management/v1/configuredevice",
            configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_TOUPTEK
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_touptek_focuser = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Focuser" &&
                entry.value("DeviceNumber", -1) == 9202) {
                EXPECT(entry.value("Vendor", "") == "touptek");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "touptek");
                EXPECT(cfg.value("deviceType", "") == "focuser");
                EXPECT(cfg.value("focuserId", "") == "tp-aaf-routing-test");
                found_touptek_focuser = true;
                break;
            }
        }
        EXPECT(found_touptek_focuser);

        nlohmann::json remove_body = {
            {"vendor", "touptek"},
            {"deviceType", "focuser"},
            {"deviceNumber", 9202}
        };
        const auto remove_response = route_request(
            router,
            "POST",
            "/management/v1/removedevice",
            remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- ToupTek AFW filter wheel routing/config persistence test ---
    // Guards against sanitize_device_config dropping the filter-wheel binding
    // and custom filter names on save (a strict allowlist silently strips any
    // field it does not copy).
#ifdef ALPACACORE_ENABLE_TOUPTEK
    {
        nlohmann::json remove_body = {{"vendor", "touptek"}, {"deviceType", "filterwheel"}, {"deviceNumber", 9203}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        const std::vector<std::string> filter_names = {"Lum", "Red", "Green", "Blue", "Ha"};
        nlohmann::json configure_body = {{"vendor", "touptek"},
                                         {"deviceType", "filterwheel"},
                                         {"deviceNumber", 9203},
                                         {"filterwheelIndex", 0},
                                         {"filterwheelId", "tp-afw-routing-test"},
                                         {"filterNames", filter_names}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_TOUPTEK
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_touptek_wheel = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "FilterWheel" && entry.value("DeviceNumber", -1) == 9203) {
                EXPECT(entry.value("Vendor", "") == "touptek");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "touptek");
                EXPECT(cfg.value("deviceType", "") == "filterwheel");
                // The three fields that sanitize_device_config used to strip.
                EXPECT(cfg.value("filterwheelId", "") == "tp-afw-routing-test");
                EXPECT(cfg.contains("filterwheelIndex"));
                EXPECT(cfg.contains("filterNames"));
                EXPECT(cfg["filterNames"] == filter_names);
                found_touptek_wheel = true;
                break;
            }
        }
        EXPECT(found_touptek_wheel);

        nlohmann::json remove_body = {{"vendor", "touptek"}, {"deviceType", "filterwheel"}, {"deviceNumber", 9203}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- QHY integrated CFW filter wheel routing/config persistence test ---
    // Guards against sanitize_device_config dropping filterNames on save (the
    // qhy branch only allowlisted cameraIndex/cameraId until this was added).
#ifdef ALPACACORE_ENABLE_QHY
    {
        nlohmann::json remove_body = {{"vendor", "qhy"}, {"deviceType", "filterwheel"}, {"deviceNumber", 9204}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        const std::vector<std::string> filter_names = {"Lum", "Red", "Green", "Blue", "Ha"};
        nlohmann::json configure_body = {{"vendor", "qhy"},
                                         {"deviceType", "filterwheel"},
                                         {"deviceNumber", 9204},
                                         {"cameraIndex", 0},
                                         {"filterNames", filter_names}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_QHY
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_qhy_wheel = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "FilterWheel" && entry.value("DeviceNumber", -1) == 9204) {
                EXPECT(entry.value("Vendor", "") == "qhy");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "qhy");
                EXPECT(cfg.value("deviceType", "") == "filterwheel");
                EXPECT(cfg.contains("cameraIndex"));
                EXPECT(cfg.contains("filterNames"));
                EXPECT(cfg["filterNames"] == filter_names);
                found_qhy_wheel = true;
                break;
            }
        }
        EXPECT(found_qhy_wheel);

        nlohmann::json remove_body = {{"vendor", "qhy"}, {"deviceType", "filterwheel"}, {"deviceNumber", 9204}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- iOptron iMate PowerBox switch routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_IOPTRON
    {
        nlohmann::json remove_body = {{"vendor", "ioptron"}, {"deviceType", "switch"}, {"deviceNumber", 9301}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        nlohmann::json configure_body = {{"vendor", "ioptron"},
                                         {"deviceType", "switch"},
                                         {"deviceNumber", 9301},
                                         {"gpioChip", "/dev/gpiochip1"},
                                         {"pwmFrequencyHz", 2000},
                                         // Positional DC3/DC1/DC2 overlay: DC1 dimmable, DC2 plain on/off.
                                         {"ports", {nlohmann::json::object(), {{"pwm", true}}, {{"pwm", false}}}}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#if defined(ALPACACORE_ENABLE_IOPTRON) && defined(ALPACACORE_IOPTRON_POWERBOX)
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_powerbox = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Switch" && entry.value("DeviceNumber", -1) == 9301) {
                EXPECT(entry.value("Vendor", "") == "ioptron");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "ioptron");
                EXPECT(cfg.value("deviceType", "") == "switch");
                // The iMate PowerBox persists the GPIO chip override plus the
                // PWM frequency and per-port PWM overlay; the mount connection
                // fields must NOT leak into a switch config.
                EXPECT(cfg.value("gpioChip", "") == "/dev/gpiochip1");
                EXPECT(cfg.value("pwmFrequencyHz", 0) == 2000);
                EXPECT(cfg.contains("ports"));
                EXPECT(cfg["ports"].is_array());
                EXPECT(cfg["ports"].size() == 3);
                EXPECT(cfg["ports"][1].value("pwm", false) == true);
                EXPECT(cfg["ports"][2].value("pwm", true) == false);
                EXPECT(!cfg.contains("connectionType"));
                EXPECT(!cfg.contains("portPath"));
                found_powerbox = true;
                break;
            }
        }
        EXPECT(found_powerbox);

        // MaxSwitch reports the three DC outputs without needing hardware.
        const auto maxswitch_response = route_request(router, "GET", "/api/v1/switch/9301/maxswitch");
        const auto maxswitch_json = nlohmann::json::parse(maxswitch_response.body());
        EXPECT(maxswitch_json.value("ErrorNumber", -1) == 0);
        EXPECT(maxswitch_json.value("Value", -1) == 3);

        nlohmann::json remove_body = {{"vendor", "ioptron"}, {"deviceType", "switch"}, {"deviceNumber", 9301}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- WandererAstro CoverCalibrator routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
    {
        nlohmann::json remove_body = {
            {"vendor", "wandererastro"}, {"deviceType", "covercalibrator"}, {"deviceNumber", 9401}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        // Serial mode with an explicit (dummy) port avoids the auto-detect scan
        // and registers the driver without opening a real device.
        nlohmann::json configure_body = {{"vendor", "wandererastro"}, {"deviceType", "covercalibrator"},
                                         {"deviceNumber", 9401},      {"connectionType", "serial"},
                                         {"portPath", "/dev/null"},   {"baudRate", 19200}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_cover = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "CoverCalibrator" && entry.value("DeviceNumber", -1) == 9401) {
                EXPECT(entry.value("Vendor", "") == "wandererastro");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "wandererastro");
                EXPECT(cfg.value("deviceType", "") == "covercalibrator");
                EXPECT(cfg.value("connectionType", "") == "serial");
                EXPECT(cfg.value("portPath", "") == "/dev/null");
                EXPECT(cfg.value("baudRate", -1) == 19200);
                found_cover = true;
                break;
            }
        }
        EXPECT(found_cover);

        // MaxBrightness is a static capability and reports without hardware.
        const auto maxbright_response = route_request(router, "GET", "/api/v1/covercalibrator/9401/maxbrightness");
        const auto maxbright_json = nlohmann::json::parse(maxbright_response.body());
        EXPECT(maxbright_json.value("ErrorNumber", -1) == 0);
        EXPECT(maxbright_json.value("Value", -1) == 255);

        nlohmann::json remove_body = {
            {"vendor", "wandererastro"}, {"deviceType", "covercalibrator"}, {"deviceNumber", 9401}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- WandererAstro Rotator routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
    {
        nlohmann::json remove_body = {{"vendor", "wandererastro"}, {"deviceType", "rotator"}, {"deviceNumber", 9402}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        // Serial mode with an explicit (dummy) port avoids the auto-detect scan
        // and registers the driver without opening a real device.
        nlohmann::json configure_body = {{"vendor", "wandererastro"}, {"deviceType", "rotator"},
                                         {"deviceNumber", 9402},      {"connectionType", "serial"},
                                         {"portPath", "/dev/null"},   {"baudRate", 19200}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);

        bool found_rotator = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Rotator" && entry.value("DeviceNumber", -1) == 9402) {
                EXPECT(entry.value("Vendor", "") == "wandererastro");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "wandererastro");
                EXPECT(cfg.value("deviceType", "") == "rotator");
                EXPECT(cfg.value("connectionType", "") == "serial");
                EXPECT(cfg.value("portPath", "") == "/dev/null");
                EXPECT(cfg.value("baudRate", -1) == 19200);
                found_rotator = true;
                break;
            }
        }
        EXPECT(found_rotator);

        // CanReverse and StepSize are static capabilities that report without
        // hardware (1142 steps/degree worm drive).
        const auto canreverse_response = route_request(router, "GET", "/api/v1/rotator/9402/canreverse");
        const auto canreverse_json = nlohmann::json::parse(canreverse_response.body());
        EXPECT(canreverse_json.value("ErrorNumber", -1) == 0);
        EXPECT(canreverse_json.value("Value", false) == true);

        nlohmann::json remove_body = {{"vendor", "wandererastro"}, {"deviceType", "rotator"}, {"deviceNumber", 9402}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- WandererAstro FilterWheel routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
    {
        nlohmann::json remove_body = {
            {"vendor", "wandererastro"}, {"deviceType", "filterwheel"}, {"deviceNumber", 9403}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        // Serial mode with an explicit (dummy) port avoids the auto-detect scan
        // and registers the driver without opening a real device.
        nlohmann::json configure_body = {{"vendor", "wandererastro"},
                                         {"deviceType", "filterwheel"},
                                         {"deviceNumber", 9403},
                                         {"connectionType", "serial"},
                                         {"portPath", "/dev/null"},
                                         {"baudRate", 19200},
                                         {"filterNames", {"L", "R", "G", "B", "Ha", "OIII", "SII", "Clear"}}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);

        bool found_filterwheel = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "FilterWheel" && entry.value("DeviceNumber", -1) == 9403) {
                EXPECT(entry.value("Vendor", "") == "wandererastro");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "wandererastro");
                EXPECT(cfg.value("deviceType", "") == "filterwheel");
                EXPECT(cfg.value("connectionType", "") == "serial");
                EXPECT(cfg.value("portPath", "") == "/dev/null");
                EXPECT(cfg.value("baudRate", -1) == 19200);
                EXPECT(cfg.contains("filterNames"));
                EXPECT(cfg["filterNames"].size() == 8);
                EXPECT(cfg["filterNames"][4] == "Ha");
                found_filterwheel = true;
                break;
            }
        }
        EXPECT(found_filterwheel);

        // Names and FocusOffsets are driver-side state that report without
        // hardware (the whole Wanderer lineup is fixed at 8 slots).
        const auto names_response = route_request(router, "GET", "/api/v1/filterwheel/9403/names");
        const auto names_json = nlohmann::json::parse(names_response.body());
        EXPECT(names_json.value("ErrorNumber", -1) == 0);
        EXPECT(names_json["Value"].size() == 8);
        EXPECT(names_json["Value"][0] == "L");

        nlohmann::json remove_body = {
            {"vendor", "wandererastro"}, {"deviceType", "filterwheel"}, {"deviceNumber", 9403}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- WandererAstro WandererBox Pro V3 Switch routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
    {
        nlohmann::json remove_body = {{"vendor", "wandererastro"}, {"deviceType", "switch"}, {"deviceNumber", 9404}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        // Serial mode with an explicit (dummy) port avoids the auto-detect scan
        // and registers the driver without opening a real device.
        nlohmann::json configure_body = {{"vendor", "wandererastro"},  {"deviceType", "switch"},
                                         {"deviceNumber", 9404},       {"switchType", "wandererbox-pro-v3"},
                                         {"connectionType", "serial"}, {"portPath", "/dev/null"},
                                         {"baudRate", 19200}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);

        bool found_switch = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Switch" && entry.value("DeviceNumber", -1) == 9404) {
                EXPECT(entry.value("Vendor", "") == "wandererastro");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "wandererastro");
                EXPECT(cfg.value("deviceType", "") == "switch");
                EXPECT(cfg.value("switchType", "") == "wandererbox-pro-v3");
                EXPECT(cfg.value("connectionType", "") == "serial");
                EXPECT(cfg.value("portPath", "") == "/dev/null");
                EXPECT(cfg.value("baudRate", -1) == 19200);
                found_switch = true;
                break;
            }
        }
        EXPECT(found_switch);

        // MaxSwitch is a static capability that reports without hardware (the
        // Pro V3 exposes 14 outputs + 10 sensor values).
        const auto maxswitch_response = route_request(router, "GET", "/api/v1/switch/9404/maxswitch");
        const auto maxswitch_json = nlohmann::json::parse(maxswitch_response.body());
        EXPECT(maxswitch_json.value("ErrorNumber", -1) == 0);
        EXPECT(maxswitch_json.value("Value", -1) == 24);

        // An unknown switchType must be rejected with a clear error.
        nlohmann::json bad_body = {{"vendor", "wandererastro"},
                                   {"deviceType", "switch"},
                                   {"deviceNumber", 9405},
                                   {"switchType", "not-a-backend"}};
        const auto bad_response = route_request(router, "POST", "/management/v1/configuredevice", bad_body.dump());
        const auto bad_json = nlohmann::json::parse(bad_response.body());
        EXPECT(bad_json.value("ErrorNumber", 0) != 0);

        nlohmann::json remove_body = {{"vendor", "wandererastro"}, {"deviceType", "switch"}, {"deviceNumber", 9404}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // =====================================================================
    // Issue #102 back-fill: config save->load round-trips for every
    // (vendor, deviceType) that persists fields. Each block POSTs distinctive
    // values, reads configureddevices back, and asserts EVERY persisted field
    // survived sanitize_device_config. Required Test Case #6 for each driver.
    // Device numbers 96xx.
    // =====================================================================

#ifdef ALPACACORE_ENABLE_ZWO
    {
        // zwo / camera
        const auto cfg = roundtrip_config(
            router,
            {{"vendor", "zwo"}, {"deviceType", "camera"}, {"deviceNumber", 9601}, {"cameraIndex", 1}, {"cameraId", 7}},
            "Camera", 9601);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("cameraIndex", -1) == 1);
        EXPECT(cfg.value("cameraId", -1) == 7);
        remove_device(router, "zwo", "camera", 9601);
    }
    {
        // zwo / filterwheel
        const auto cfg =
            roundtrip_config(router,
                             {{"vendor", "zwo"},
                              {"deviceType", "filterwheel"},
                              {"deviceNumber", 9602},
                              {"filterwheelIndex", 1},
                              {"filterwheelId", 5},
                              {"filterNames", nlohmann::json::array({"Lum", "Red", "Green", "Blue", "Ha"})}},
                             "FilterWheel", 9602);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("filterwheelIndex", -1) == 1);
        EXPECT(cfg.value("filterwheelId", -1) == 5);
        EXPECT(cfg.contains("filterNames"));
        EXPECT(cfg["filterNames"].size() == 5);
        EXPECT(cfg["filterNames"][4] == "Ha");
        remove_device(router, "zwo", "filterwheel", 9602);
    }
    {
        // zwo / focuser
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "zwo"},
                                           {"deviceType", "focuser"},
                                           {"deviceNumber", 9603},
                                           {"focuserIndex", 1},
                                           {"focuserId", 3}},
                                          "Focuser", 9603);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("focuserIndex", -1) == 1);
        EXPECT(cfg.value("focuserId", -1) == 3);
        remove_device(router, "zwo", "focuser", 9603);
    }
    {
        // zwo / rotator
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "zwo"},
                                           {"deviceType", "rotator"},
                                           {"deviceNumber", 9604},
                                           {"rotatorIndex", 1},
                                           {"rotatorId", 2}},
                                          "Rotator", 9604);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("rotatorIndex", -1) == 1);
        EXPECT(cfg.value("rotatorId", -1) == 2);
        remove_device(router, "zwo", "rotator", 9604);
    }
    {
        // zwo / switch (dew heater — the default switchType)
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "zwo"},
                                           {"deviceType", "switch"},
                                           {"deviceNumber", 9605},
                                           {"switchType", "dewheater"},
                                           {"cameraIndex", 1},
                                           {"cameraId", 4}},
                                          "Switch", 9605);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("switchType", "") == "dewheater");
        EXPECT(cfg.value("cameraIndex", -1) == 1);
        EXPECT(cfg.value("cameraId", -1) == 4);
        remove_device(router, "zwo", "switch", 9605);
    }
    {
        // zwo / switch (ASIAIR Pro/CM4 — libgpiod backend): gpioChip +
        // pwmFrequencyHz + per-port gpio/name/pwm must all survive (the ports
        // array is copied wholesale; a deep-filter regression would strip gpio).
        nlohmann::json ports = nlohmann::json::array(
            {{{"gpio", 12}, {"name", "Mount"}, {"pwm", false}}, {{"gpio", 13}, {"name", "Dew Heater"}, {"pwm", true}}});
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "zwo"},
                                           {"deviceType", "switch"},
                                           {"deviceNumber", 9606},
                                           {"switchType", "asiair"},
                                           {"gpioChip", "/dev/gpiochip0"},
                                           {"pwmFrequencyHz", 200},
                                           {"ports", ports}},
                                          "Switch", 9606);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("switchType", "") == "asiair");
        EXPECT(cfg.value("gpioChip", "") == "/dev/gpiochip0");
        EXPECT(cfg.value("pwmFrequencyHz", -1) == 200);
        EXPECT(cfg.contains("ports"));
        EXPECT(cfg["ports"].size() == 2);
        EXPECT(cfg["ports"][0].value("gpio", -1) == 12);
        EXPECT(cfg["ports"][1].value("name", "") == "Dew Heater");
        EXPECT(cfg["ports"][1].value("pwm", false) == true);
        remove_device(router, "zwo", "switch", 9606);
    }
    {
        // zwo / switch (ASIAIR Plus RK3568 — kernel-module backend):
        // devicePath instead of gpioChip; ports entries carry name/pwm only.
        nlohmann::json ports = nlohmann::json::array({{{"name", "DC1"}, {"pwm", true}}});
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "zwo"},
                                           {"deviceType", "switch"},
                                           {"deviceNumber", 9607},
                                           {"switchType", "asiair-plus-rk3568"},
                                           {"devicePath", "/dev/pwm-gpio-misc"},
                                           {"pwmFrequencyHz", 50},
                                           {"ports", ports}},
                                          "Switch", 9607);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("switchType", "") == "asiair-plus-rk3568");
        EXPECT(cfg.value("devicePath", "") == "/dev/pwm-gpio-misc");
        EXPECT(cfg.value("pwmFrequencyHz", -1) == 50);
        EXPECT(cfg.contains("ports"));
        EXPECT(cfg["ports"][0].value("pwm", false) == true);
        // The gpioChip key belongs to the libgpiod variants only.
        EXPECT(!cfg.contains("gpioChip"));
        remove_device(router, "zwo", "switch", 9607);
    }
#endif

#ifdef ALPACACORE_ENABLE_QHY
    {
        // qhy / camera — cameraId is a STRING for QHY (char[32] ids).
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "qhy"},
                                           {"deviceType", "camera"},
                                           {"deviceNumber", 9608},
                                           {"cameraIndex", 1},
                                           {"cameraId", "QHY-TEST-1"}},
                                          "Camera", 9608);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("cameraIndex", -1) == 1);
        EXPECT(cfg.value("cameraId", "") == "QHY-TEST-1");
        remove_device(router, "qhy", "camera", 9608);
    }
#endif

#ifdef ALPACACORE_ENABLE_SVBONY
    {
        // svbony / camera
        const auto cfg = roundtrip_config(
            router, {{"vendor", "svbony"}, {"deviceType", "camera"}, {"deviceNumber", 9609}, {"cameraIndex", 2}},
            "Camera", 9609);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("cameraIndex", -1) == 2);
        remove_device(router, "svbony", "camera", 9609);
    }
#endif

#if defined(ALPACACORE_ENABLE_TOUPTEK) && defined(ALPACACORE_TOUPTEK_STELLAVITA)
    {
        // touptek / switch (StellaVita PowerBox) — field survival, not just
        // the existing no-crash test.
        nlohmann::json ports =
            nlohmann::json::array({{{"name", "Flat Panel"}, {"pwm", true}}, {{"name", "Camera"}, {"pwm", false}}});
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "touptek"},
                                           {"deviceType", "switch"},
                                           {"deviceNumber", 9610},
                                           {"switchType", "stellavita"},
                                           {"gpioChip", "/dev/gpiochip0"},
                                           {"pwmFrequencyHz", 100},
                                           {"ports", ports}},
                                          "Switch", 9610);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("switchType", "") == "stellavita");
        EXPECT(cfg.value("gpioChip", "") == "/dev/gpiochip0");
        EXPECT(cfg.value("pwmFrequencyHz", -1) == 100);
        EXPECT(cfg.contains("ports"));
        EXPECT(cfg["ports"][0].value("pwm", false) == true);
        remove_device(router, "touptek", "switch", 9610);
    }
#endif

#ifdef ALPACACORE_ENABLE_PLAYERONE
    {
        // playerone / camera — full round-trip (previous test was configure-only).
        const auto cfg = roundtrip_config(
            router, {{"vendor", "playerone"}, {"deviceType", "camera"}, {"deviceNumber", 9611}, {"cameraIndex", 3}},
            "Camera", 9611);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("cameraIndex", -1) == 3);
        remove_device(router, "playerone", "camera", 9611);
    }
    {
        // playerone / filterwheel
        const auto cfg =
            roundtrip_config(router,
                             {{"vendor", "playerone"},
                              {"deviceType", "filterwheel"},
                              {"deviceNumber", 9612},
                              {"filterwheelIndex", 1},
                              {"filterNames", nlohmann::json::array({"Lum", "Red", "Green", "Blue", "Ha"})}},
                             "FilterWheel", 9612);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("filterwheelIndex", -1) == 1);
        EXPECT(cfg.contains("filterNames"));
        EXPECT(cfg["filterNames"].size() == 5);
        remove_device(router, "playerone", "filterwheel", 9612);
    }
#endif

#ifdef ALPACACORE_ENABLE_GEMINI
    {
        // gemini / focuser
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "gemini"},
                                           {"deviceType", "focuser"},
                                           {"deviceNumber", 9613},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB7"},
                                           {"baudRate", 19200},
                                           {"focuserIndex", 1}},
                                          "Focuser", 9613);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB7");
        EXPECT(cfg.value("baudRate", -1) == 19200);
        EXPECT(cfg.value("focuserIndex", -1) == 1);
        remove_device(router, "gemini", "focuser", 9613);
    }
    {
        // gemini / covercalibrator (Flat Panel Cover Lite) — shares the vendor
        // config block with the focuser above; guards panelIndex persistence
        // through sanitize_device_config.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "gemini"},
                                           {"deviceType", "covercalibrator"},
                                           {"deviceNumber", 9618},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB8"},
                                           {"baudRate", 19200},
                                           {"panelIndex", 2}},
                                          "CoverCalibrator", 9618);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB8");
        EXPECT(cfg.value("baudRate", -1) == 19200);
        EXPECT(cfg.value("panelIndex", -1) == 2);
        remove_device(router, "gemini", "covercalibrator", 9618);
    }
    {
        // gemini / covercalibrator (Astro Automatic FlatPanel v2, motorized
        // cover) — same vendor+deviceType slot as the Lite case above,
        // distinguished by flatPanelModel; guards that field's persistence
        // through sanitize_device_config and that it actually selects the v2
        // driver (registered device count/type is the same either way, so
        // this only proves routing didn't reject the config).
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "gemini"},
                                           {"deviceType", "covercalibrator"},
                                           {"deviceNumber", 9619},
                                           {"flatPanelModel", "v2"},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB9"},
                                           {"baudRate", 19200},
                                           {"panelIndex", 3}},
                                          "CoverCalibrator", 9619);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("flatPanelModel", "") == "v2");
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB9");
        EXPECT(cfg.value("baudRate", -1) == 19200);
        EXPECT(cfg.value("panelIndex", -1) == 3);
        remove_device(router, "gemini", "covercalibrator", 9619);
    }
#endif

#ifdef ALPACACORE_ENABLE_ASTROASIS
    {
        // astroasis / focuser — explicit hidPath persists through
        // sanitize_device_config. (An empty hidPath instead falls back to
        // focuserIndex, which eagerly scans the USB bus at construction and
        // has no lazy no-hardware path to round-trip in this test.)
        const auto cfg = roundtrip_config(
            router,
            {{"vendor", "astroasis"}, {"deviceType", "focuser"}, {"deviceNumber", 9621}, {"hidPath", "/dev/hidraw3"}},
            "Focuser", 9621);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("hidPath", "") == "/dev/hidraw3");
        remove_device(router, "astroasis", "focuser", 9621);
    }
#endif

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
    {
        // wandererastro / rotator (WandererRotator Mini) — auto mode persists
        // rotatorIndex through sanitize_device_config.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "wandererastro"},
                                           {"deviceType", "rotator"},
                                           {"deviceNumber", 9619},
                                           {"connectionType", "auto"},
                                           {"rotatorIndex", 1}},
                                          "Rotator", 9619);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "auto");
        EXPECT(cfg.value("rotatorIndex", -1) == 1);
        remove_device(router, "wandererastro", "rotator", 9619);
    }

    {
        // wandererastro / switch (WandererBox Pro V3) — auto mode persists
        // switchType and boxIndex through sanitize_device_config.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "wandererastro"},
                                           {"deviceType", "switch"},
                                           {"deviceNumber", 9620},
                                           {"switchType", "wandererbox-pro-v3"},
                                           {"connectionType", "auto"},
                                           {"boxIndex", 1}},
                                          "Switch", 9620);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("switchType", "") == "wandererbox-pro-v3");
        EXPECT(cfg.value("connectionType", "") == "auto");
        EXPECT(cfg.value("boxIndex", -1) == 1);
        remove_device(router, "wandererastro", "switch", 9620);
    }
#endif

#ifdef ALPACACORE_ENABLE_WEEWX
    {
        // weewx / observingconditions
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "weewx"},
                                           {"deviceType", "observingconditions"},
                                           {"deviceNumber", 9614},
                                           {"weewxUrl", "http://weewx.test:8998/current.json"},
                                           {"pollIntervalSeconds", 300},
                                           {"timeoutMs", 2500}},
                                          "ObservingConditions", 9614);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("weewxUrl", "") == "http://weewx.test:8998/current.json");
        EXPECT(cfg.value("pollIntervalSeconds", -1) == 300);
        EXPECT(cfg.value("timeoutMs", -1) == 2500);
        remove_device(router, "weewx", "observingconditions", 9614);
    }
#endif

#ifdef ALPACACORE_ENABLE_IOPTRON
    {
        // ioptron / telescope — asserts mountIndex survival: it is read by the
        // auto-detect registration path but was missing from the sanitizer
        // allowlist until issue #102 (saved index silently reverted to 0).
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "ioptron"},
                                           {"deviceType", "telescope"},
                                           {"deviceNumber", 9615},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB6"},
                                           {"baudRate", 115200},
                                           {"mountIndex", 1}},
                                          "Telescope", 9615);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB6");
        EXPECT(cfg.value("baudRate", -1) == 115200);
        EXPECT(cfg.value("mountIndex", -1) == 1);
        remove_device(router, "ioptron", "telescope", 9615);
    }
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
    {
        // synscan / telescope — same mountIndex gap as ioptron; also the
        // synscanVersion discriminator must survive.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "synscan"},
                                           {"deviceType", "telescope"},
                                           {"deviceNumber", 9616},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB5"},
                                           {"baudRate", 9600},
                                           {"synscanVersion", "v4"},
                                           {"mountIndex", 2}},
                                          "Telescope", 9616);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB5");
        EXPECT(cfg.value("baudRate", -1) == 9600);
        EXPECT(cfg.value("synscanVersion", "") == "v4");
        EXPECT(cfg.value("mountIndex", -1) == 2);
        remove_device(router, "synscan", "telescope", 9616);
    }
#endif

#ifdef ALPACACORE_ENABLE_BISQUE
    {
        // bisque / telescope (TheSkyX TCP)
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "bisque"},
                                           {"deviceType", "telescope"},
                                           {"deviceNumber", 9617},
                                           {"host", "skyx.test"},
                                           {"tcpPort", 3041}},
                                          "Telescope", 9617);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("host", "") == "skyx.test");
        EXPECT(cfg.value("tcpPort", -1) == 3041);
        remove_device(router, "bisque", "telescope", 9617);
    }
#endif

    // configureddevices surfaces Firmware and SdkVersion independently, each only
    // when the live driver reports that specific value.
    {
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        // Real device firmware only (e.g. WandererCover / a mount).
        auto firmware_dev = std::make_shared<FirmwareStubDriver>(9501, std::string("2025-05-04"));
        // Vendor SDK version only (e.g. ZWO camera — no device firmware API).
        auto sdk_dev = std::make_shared<FirmwareStubDriver>(9502, std::nullopt, std::string("1.7.7.0"));
        // Neither.
        auto silent = std::make_shared<FirmwareStubDriver>(9503, std::nullopt);
        EXPECT(registry.register_device(firmware_dev));
        EXPECT(registry.register_device(sdk_dev));
        EXPECT(registry.register_device(silent));

        const auto response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto json = nlohmann::json::parse(response.body());
        EXPECT(json.value("ErrorNumber", -1) == 0);

        bool checked_firmware = false;
        bool checked_sdk = false;
        bool checked_silent = false;
        for (const auto& entry : json["Value"]) {
            if (entry.value("DeviceType", "") != "CoverCalibrator") {
                continue;
            }
            if (entry.value("DeviceNumber", -1) == 9501) {
                EXPECT(entry.contains("Firmware"));
                EXPECT(entry.value("Firmware", "") == "2025-05-04");
                EXPECT(!entry.contains("SdkVersion"));
                checked_firmware = true;
            } else if (entry.value("DeviceNumber", -1) == 9502) {
                EXPECT(!entry.contains("Firmware"));
                EXPECT(entry.contains("SdkVersion"));
                EXPECT(entry.value("SdkVersion", "") == "1.7.7.0");
                checked_sdk = true;
            } else if (entry.value("DeviceNumber", -1) == 9503) {
                EXPECT(!entry.contains("Firmware"));
                EXPECT(!entry.contains("SdkVersion"));
                checked_silent = true;
            }
        }
        EXPECT(checked_firmware);
        EXPECT(checked_sdk);
        EXPECT(checked_silent);

        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9501);
        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9502);
        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9503);
    }

    // Issue #160: per-client Connected refcounting. Two clients sharing one
    // device (imaging app + guider on the same mount): the first client in
    // powers the upstream link, the last one out tears it down, and one
    // client's disconnect must never take the device away from the other.
    {
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto stub = std::make_shared<ConnectStubDriver>(9701);
        EXPECT(registry.register_device(stub));
        const std::string base = "/api/v1/covercalibrator/9701";

        // Before anyone connects: false for everyone.
        EXPECT(!get_connected_value(router, base, "1"));
        EXPECT(!get_connected_value(router, base, ""));

        // Client 1 connects: the device link comes up exactly once.
        put_connected(router, base, "1", true);
        EXPECT(stub->connect_count == 1);
        EXPECT(get_connected_value(router, base, "1"));

        // A client that never connected reads false even though the device is
        // up; a ClientID-less probe reads raw device state (legacy behavior).
        EXPECT(!get_connected_value(router, base, "2"));
        EXPECT(get_connected_value(router, base, ""));

        // Client 2 joins: no second upstream connect.
        put_connected(router, base, "2", true);
        EXPECT(stub->connect_count == 1);
        EXPECT(get_connected_value(router, base, "2"));

        // Client 2 leaves: the device MUST stay up for client 1 (the bug in
        // issue #160 tore it down here).
        put_connected(router, base, "2", false);
        EXPECT(stub->disconnect_count == 0);
        EXPECT(stub->get_connected());
        EXPECT(get_connected_value(router, base, "1"));
        EXPECT(!get_connected_value(router, base, "2"));

        // Last client out: now the link is torn down.
        put_connected(router, base, "1", false);
        EXPECT(stub->disconnect_count == 1);
        EXPECT(!stub->get_connected());

        // Disconnecting a client that was never registered on a live device
        // must not touch the link.
        put_connected(router, base, "1", true);
        EXPECT(stub->connect_count == 2);
        put_connected(router, base, "99", false);
        EXPECT(stub->disconnect_count == 1);
        EXPECT(stub->get_connected());

        // Upstream failure: the link dies underneath the bridge. Every
        // client's registration is invalidated so all observers see the
        // disconnect, and a reconnect works from a clean slate.
        stub->drop_link();
        EXPECT(!get_connected_value(router, base, "1"));
        put_connected(router, base, "1", true);
        EXPECT(stub->connect_count == 3);
        EXPECT(get_connected_value(router, base, "1"));
        put_connected(router, base, "1", false);
        EXPECT(stub->disconnect_count == 2);

        // Platform 7 connect/disconnect endpoints share the same refcount.
        route_request(router, "PUT", base + "/connect", "ClientID=1");
        route_request(router, "PUT", base + "/connect", "ClientID=2");
        EXPECT(stub->connect_count == 4);
        route_request(router, "PUT", base + "/disconnect", "ClientID=1");
        EXPECT(stub->get_connected());
        route_request(router, "PUT", base + "/disconnect", "ClientID=2");
        EXPECT(!stub->get_connected());

        // JSON PUT bodies carry ClientID too (numeric JSON ClientID).
        const auto resp = route_request(router, "PUT", base + "/connected", R"({"Connected": true, "ClientID": 7})");
        const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
        EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
        EXPECT(get_connected_value(router, base, "7"));
        EXPECT(!get_connected_value(router, base, "8"));
        put_connected(router, base, "7", false);
        EXPECT(!stub->get_connected());

        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9701);
    }

    // Issue #163: the client key is qualified by peer address, so two clients
    // that omit ClientID (or reuse the same one) on DIFFERENT hosts get
    // distinct registry slots and can no longer shadow-disconnect each other.
    {
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto stub = std::make_shared<ConnectStubDriver>(9702);
        EXPECT(registry.register_device(stub));
        const std::string base = "/api/v1/covercalibrator/9702";

        // Two anonymous (no-ClientID) clients on different hosts.
        put_connected(router, base, "", true, "10.0.0.1");
        put_connected(router, base, "", true, "10.0.0.2");
        EXPECT(stub->connect_count == 1);

        // Host 2's anonymous disconnect must not drop host 1's link (the
        // pre-#163 shared anonymous slot did exactly that).
        put_connected(router, base, "", false, "10.0.0.2");
        EXPECT(stub->disconnect_count == 0);
        EXPECT(stub->get_connected());

        // Last anonymous client out tears it down.
        put_connected(router, base, "", false, "10.0.0.1");
        EXPECT(stub->disconnect_count == 1);

        // Same ClientID from different hosts are distinct clients too, and
        // GET answers per (ClientID, host).
        put_connected(router, base, "5", true, "10.0.0.1");
        put_connected(router, base, "5", true, "10.0.0.2");
        EXPECT(stub->connect_count == 2);
        EXPECT(get_connected_value(router, base, "5", "10.0.0.1"));
        put_connected(router, base, "5", false, "10.0.0.2");
        EXPECT(stub->get_connected());
        EXPECT(get_connected_value(router, base, "5", "10.0.0.1"));
        EXPECT(!get_connected_value(router, base, "5", "10.0.0.2"));
        put_connected(router, base, "5", false, "10.0.0.1");
        EXPECT(!stub->get_connected());

        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9702);
    }

    // Security: path traversal via the static-file handler must be rejected
    // (404) without leaking file contents (audit finding C1).
    {
        const char* traversal_paths[] = {"/web/../../../../etc/passwd", "/web/../secret",
                                         "/web/../../AlpacaHTTP/CMakeLists.txt", "/web/subdir/../../secret"};
        for (const char* path : traversal_paths) {
            const auto resp = route_request(router, "GET", path);
            EXPECT(resp.status_code() == 404 || resp.status_code() == 403 || resp.status_code() == 400);
            EXPECT(resp.body().find("root:") == std::string::npos);
            EXPECT(resp.body().find("cmake_minimum_required") == std::string::npos);
        }
    }

    // Security: Content-Length must be bounded and validated (audit finding
    // M1). An absurd or malformed value must fail parsing rather than drive a
    // multi-gigabyte body_.resize().
    {
        alpacahttp::Request bad_request;

        // Hostile size (about 4 GB) — over the kMaxBodyBytes cap.
        std::string oversize =
            "POST /management/v1/configuredevice HTTP/1.1\r\n"
            "Content-Length: 4294967295\r\n\r\n{}";
        EXPECT(!bad_request.parse(oversize));

        // Just over the cap.
        std::string over_cap =
            "POST /management/v1/configuredevice HTTP/1.1\r\n"
            "Content-Length: " +
            std::to_string(alpacahttp::Request::kMaxBodyBytes + 1) + "\r\n\r\n{}";
        EXPECT(!bad_request.parse(over_cap));

        // Non-numeric and overflowing values must be rejected, not ignored.
        std::string non_numeric =
            "POST /management/v1/configuredevice HTTP/1.1\r\n"
            "Content-Length: banana\r\n\r\n{}";
        EXPECT(!bad_request.parse(non_numeric));

        std::string overflow =
            "POST /management/v1/configuredevice HTTP/1.1\r\n"
            "Content-Length: 99999999999999999999999999\r\n\r\n{}";
        EXPECT(!bad_request.parse(overflow));

        // A well-formed request within the cap still parses.
        alpacahttp::Request good_request;
        std::string good =
            "POST /management/v1/configuredevice HTTP/1.1\r\n"
            "Content-Length: 2\r\n\r\n{}";
        EXPECT(good_request.parse(good));
        EXPECT(good_request.body() == "{}");
    }

    std::cout << "All routing tests passed!\n";
    return 0;
}

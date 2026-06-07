// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacahttp/request.h>
#include <alpacahttp/router.h>

#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "test_assert.h"

namespace {

alpacahttp::Response route_request(alpacahttp::Router& router,
                                   const std::string& method,
                                   const std::string& path,
                                   const std::string& body = std::string()) {
    alpacahttp::Request request;
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

    std::cout << "All routing tests passed!\n";
    return 0;
}

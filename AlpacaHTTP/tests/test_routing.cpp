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

#include <alpacahttp/router.h>
#include <alpacahttp/request.h>
#include <nlohmann/json.hpp>
#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>

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

    assert(request.parse(raw.str()));
    return router.route(request, 1);
}

} // namespace

int main() {
    std::cout << "Testing routing...\n";

    alpacahttp::Router router;
    alpacahttp::Request request;

    // Test management endpoint parsing
    std::string test_request = "GET /management/v1/description HTTP/1.1\r\n\r\n";
    assert(request.parse(test_request));
    assert(request.path() == "/management/v1/description");

    // Test device endpoint parsing
    test_request = "GET /api/v1/camera/0/canconnect HTTP/1.1\r\n\r\n";
    assert(request.parse(test_request));
    assert(request.path() == "/api/v1/camera/0/canconnect");

    // Test query parameters
    test_request = "GET /api/v1/mount/0/slewto?RightAscension=1.5&Declination=-20.3 HTTP/1.1\r\n\r\n";
    assert(request.parse(test_request));
    assert(request.path() == "/api/v1/mount/0/slewto");
    assert(request.has_query_param("RightAscension"));
    assert(request.has_query_param("Declination"));

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
        assert(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        assert(configured_json.value("ErrorNumber", -1) == 0);
        assert(configured_json.contains("Value"));
        assert(configured_json["Value"].is_array());

        bool found_device = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Telescope" &&
                entry.value("DeviceNumber", -1) == 9101) {
                assert(entry.value("Vendor", "") == "zwo");
                assert(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                assert(cfg.value("vendor", "") == "zwo");
                assert(cfg.value("deviceType", "") == "telescope");
                assert(cfg.value("connectionType", "") == "serial");
                assert(cfg.value("portPath", "") == "/dev/null");
                assert(cfg.value("responseTimeoutMs", -1) == 2500);
                assert(std::abs(cfg.value("apertureDiameter", 0.0) - 0.1) < 1e-12);
                assert(std::abs(cfg.value("focalLength", 0.0) - 0.8) < 1e-12);
                assert(std::abs(cfg.value("siteLatitude", 0.0) - 34.5) < 1e-12);
                assert(std::abs(cfg.value("siteLongitude", 0.0) - (-117.2)) < 1e-12);
                assert(std::abs(cfg.value("siteElevation", 0.0) - 450.0) < 1e-12);
                assert(cfg.value("syncTimeOnConnect", true) == false);
                found_device = true;
                break;
            }
        }
        assert(found_device);

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
        assert(remove_json.value("ErrorNumber", -1) == 0);
#else
        assert(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

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
        assert(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        assert(configured_json.value("ErrorNumber", -1) == 0);
        assert(configured_json.contains("Value"));
        assert(configured_json["Value"].is_array());

        bool found_celestron = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Telescope" &&
                entry.value("DeviceNumber", -1) == 9102) {
                assert(entry.value("Vendor", "") == "celestron");
                assert(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                assert(cfg.value("vendor", "") == "celestron");
                assert(cfg.value("deviceType", "") == "telescope");
                assert(cfg.value("connectionType", "") == "serial");
                assert(cfg.value("portPath", "") == "/dev/null");
                assert(cfg.value("baudRate", -1) == 9600);
                assert(cfg.value("responseTimeoutMs", -1) == 5000);
                assert(std::abs(cfg.value("apertureDiameter", 0.0) - 0.28) < 1e-12);
                assert(std::abs(cfg.value("focalLength", 0.0) - 2.8) < 1e-12);
                assert(std::abs(cfg.value("siteLatitude", 0.0) - 33.85) < 1e-12);
                assert(std::abs(cfg.value("siteLongitude", 0.0) - (-118.34)) < 1e-12);
                assert(std::abs(cfg.value("siteElevation", 0.0) - 100.0) < 1e-12);
                assert(cfg.value("syncTimeOnConnect", false) == true);
                found_celestron = true;
                break;
            }
        }
        assert(found_celestron);

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
        assert(net_json.value("ErrorNumber", -1) == 0);

        const auto net_configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto net_configured_json = nlohmann::json::parse(net_configured_response.body());
        bool found_net_celestron = false;
        for (const auto& entry : net_configured_json["Value"]) {
            if (entry.value("DeviceNumber", -1) == 9103) {
                const auto& cfg = entry["Config"];
                assert(cfg.value("connectionType", "") == "network");
                assert(cfg.value("host", "") == "192.168.1.100");
                assert(cfg.value("tcpPort", -1) == 2000);
                found_net_celestron = true;
                break;
            }
        }
        assert(found_net_celestron);

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
        assert(remove_json.value("ErrorNumber", -1) == 0);

        nlohmann::json remove_net_body = {
            {"vendor", "celestron"},
            {"deviceType", "telescope"},
            {"deviceNumber", 9103}
        };
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_net_body.dump());
#else
        assert(configure_json.value("ErrorNumber", 0) != 0);
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
        assert(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        assert(configured_json.value("ErrorNumber", -1) == 0);
        assert(configured_json.contains("Value"));
        assert(configured_json["Value"].is_array());

        bool found_touptek = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Camera" &&
                entry.value("DeviceNumber", -1) == 9201) {
                assert(entry.value("Vendor", "") == "touptek");
                assert(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                assert(cfg.value("vendor", "") == "touptek");
                assert(cfg.value("deviceType", "") == "camera");
                assert(cfg.value("cameraIndex", -1) == 2);
                found_touptek = true;
                break;
            }
        }
        assert(found_touptek);

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
        assert(remove_json.value("ErrorNumber", -1) == 0);
#else
        assert(configure_json.value("ErrorNumber", 0) != 0);
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
        assert(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        assert(configured_json.value("ErrorNumber", -1) == 0);
        assert(configured_json.contains("Value"));
        assert(configured_json["Value"].is_array());

        bool found_touptek_focuser = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Focuser" &&
                entry.value("DeviceNumber", -1) == 9202) {
                assert(entry.value("Vendor", "") == "touptek");
                assert(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                assert(cfg.value("vendor", "") == "touptek");
                assert(cfg.value("deviceType", "") == "focuser");
                assert(cfg.value("focuserId", "") == "tp-aaf-routing-test");
                found_touptek_focuser = true;
                break;
            }
        }
        assert(found_touptek_focuser);

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
        assert(remove_json.value("ErrorNumber", -1) == 0);
#else
        assert(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    std::cout << "All routing tests passed!\n";
    return 0;
}

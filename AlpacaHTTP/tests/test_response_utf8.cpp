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

// Issue #764: Response::set_body(const AlpacaResponse&) must not throw when a
// JSON string holds bytes that are not valid UTF-8 (a Wi-Fi SSID is an
// arbitrary octet string). Invalid bytes become U+FFFD and every valid entry
// survives.

#include <alpacahttp/response.h>
#include <alpacahttp/wifi_manager.h>

#include <iostream>
#include <string>

#include "test_assert.h"

using alpacahttp::AlpacaResponse;
using alpacahttp::Response;

static nlohmann::json net(const std::string& ssid, int signal) {
    return nlohmann::json{{"Ssid", ssid},
                          {"SsidHex", alpacahttp::util::ssid_to_hex(ssid)},
                          {"FrequencyMhz", 2412u},
                          {"SignalPercent", signal},
                          {"Security", "WPA2"}};
}

int main() {
    std::cout << std::unitbuf;

    // Control: valid UTF-8 SSIDs serialise.
    {
        AlpacaResponse ok(1, 2);
        ok.value = nlohmann::json::array({net("HomeWiFi", 80), net("Caf\xC3\xA9", 50)});
        Response r;
        bool threw = false;
        try {
            r.set_body(ok);
        } catch (const std::exception&) {
            threw = true;
        }
        EXPECT(!threw);
        EXPECT(r.body().find("HomeWiFi") != std::string::npos);
        // Valid multi-byte UTF-8 passes through byte for byte (not \u-escaped).
        EXPECT(r.body().find("Caf\xC3\xA9") != std::string::npos);
        std::cout << "control OK, body bytes=" << r.body().size() << "\n";
    }

    // A scan containing one non-UTF-8 SSID must still serialise.
    {
        AlpacaResponse scan(1, 2);
        scan.value = nlohmann::json::array(
            {net("HomeWiFi", 80), net(std::string("\xff", 1), 60), net(std::string("\xfe", 1), 55)});
        Response r;
        bool threw = false;
        try {
            r.set_body(scan);
        } catch (const std::exception& e) {
            threw = true;
            std::cout << "set_body threw: " << e.what() << "\n";
        }
        EXPECT(!threw);
        EXPECT(r.body().find("HomeWiFi") != std::string::npos);
        EXPECT(r.body().find("\xEF\xBF\xBD") != std::string::npos);

        bool parsed_ok = true;
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(r.body());
        } catch (const std::exception&) {
            parsed_ok = false;
        }
        EXPECT(parsed_ok);
        EXPECT(parsed["Value"].size() == 3);
        EXPECT(parsed["Value"][0]["Ssid"] == "HomeWiFi");
        EXPECT(parsed["Value"][1]["Ssid"].get<std::string>() == "\xEF\xBF\xBD");
        EXPECT(parsed["Value"][1]["SsidHex"] == "ff");
        EXPECT(parsed["Value"][2]["Ssid"] == parsed["Value"][1]["Ssid"]);
        EXPECT(parsed["Value"][2]["SsidHex"] == "fe");
    }

    // Mixed: valid bytes around an invalid one survive; only the bad byte is replaced.
    {
        AlpacaResponse mixed(1, 2);
        mixed.value = nlohmann::json::array({net("Caf\xE9 Bar", 70)});
        Response r;
        bool threw = false;
        try {
            r.set_body(mixed);
        } catch (const std::exception&) {
            threw = true;
        }
        EXPECT(!threw);
        EXPECT(r.body().find("Caf\xEF\xBF\xBD Bar") != std::string::npos);
    }

    std::cout << "PASS\n";
    return 0;
}

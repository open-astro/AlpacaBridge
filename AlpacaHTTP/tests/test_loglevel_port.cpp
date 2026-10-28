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

// open-astro#1756 items 2-4: one log-level spelling table for the config file,
// ALPACAHTTP_LOG_LEVEL and the API; ALPACAHTTP_PORT range; a non-string loglevel.

#include <alpacahttp/config.h>
#include <alpacahttp/request.h>
#include <alpacahttp/router.h>

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include "test_assert.h"

namespace {

using alpacahttp::LogLevel;

LogLevel level_from_file(const std::string& spelling) {
    std::ofstream("loglevel_cfg.yaml") << "logging:\n  level: " << spelling << "\n";
    alpacahttp::Config c;
    c.load("loglevel_cfg.yaml");
    return c.log_level();
}

LogLevel level_from_env(const std::string& spelling) {
    ::setenv("ALPACAHTTP_LOG_LEVEL", spelling.c_str(), 1);
    std::ofstream("loglevel_empty.yaml") << "server:\n  name: x\n";
    alpacahttp::Config c;
    c.load("loglevel_empty.yaml");
    ::unsetenv("ALPACAHTTP_LOG_LEVEL");
    return c.log_level();
}

int port_from_env(const char* value) {
    ::setenv("ALPACAHTTP_PORT", value, 1);
    std::ofstream("port_cfg.yaml") << "http:\n  port: 1234\n";
    alpacahttp::Config c;
    c.load("port_cfg.yaml");
    ::unsetenv("ALPACAHTTP_PORT");
    return c.http_port();
}

int post_level(alpacahttp::Router& router, const std::string& body) {
    alpacahttp::Request request;
    std::ostringstream raw;
    raw << "POST /management/v1/loglevel HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n"
        << body;
    EXPECT(request.parse(raw.str()));
    return nlohmann::json::parse(router.route(request, 1).body()).value("ErrorNumber", -1);
}

}  // namespace

int main() {
    ::unsetenv("ALPACAHTTP_LOG_LEVEL");
    ::unsetenv("ALPACAHTTP_PORT");

    {
        // case: log level spellings load from the config file and the env var
        for (const auto& [spelling, want] : {std::pair<const char*, LogLevel>{"warn", LogLevel::WARNING},
                                             {"WARN", LogLevel::WARNING},
                                             {"warning", LogLevel::WARNING},
                                             {"trace", LogLevel::DEBUG},
                                             {"critical", LogLevel::ERROR},
                                             {"fatal", LogLevel::ERROR},
                                             {"error", LogLevel::ERROR}}) {
            EXPECT(level_from_file(spelling) == want);
            EXPECT(level_from_env(spelling) == want);
        }
        EXPECT(level_from_file("bogus") == LogLevel::INFO);
    }

    {
        // case: ALPACAHTTP_PORT outside 1-65535 is rejected
        EXPECT(port_from_env("70000") == 1234);
        EXPECT(port_from_env("-1") == 1234);
        EXPECT(port_from_env("6800") == 6800);
    }

    {
        // case: loglevel API accepts the shared spellings and rejects a non-string level
        alpacahttp::Router router;
        for (const char* ok : {"{\"level\":\"warn\"}", "{\"level\":\"trace\"}", "{\"level\":\"critical\"}"}) {
            EXPECT(post_level(router, ok) == 0);
        }
        EXPECT(post_level(router, "{\"level\":\"bogus\"}") == 0x401);
        for (const char* bad : {"{\"level\":3}", "{\"level\":true}", "{\"level\":null}", "{\"Level\":3}"}) {
            EXPECT(post_level(router, bad) == 0x401);
        }
    }
    return 0;
}

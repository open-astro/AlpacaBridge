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

// Settings files survive a failed write and a corrupt read (open-astro#1688,
// #1689). A write error is forced with RLIMIT_FSIZE and SIGXFSZ ignored, both
// restored afterwards; never point a path at /dev/full, /dev/zero or a FIFO.

#include <alpacahttp/request.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/logging_adapter.h>
#include <sys/resource.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "test_assert.h"
#include "test_catalog_descriptor.h"

namespace {

namespace fs = std::filesystem;

struct FileSizeLimit {
    rlimit saved{};
    void (*saved_handler)(int);
    explicit FileSizeLimit(rlim_t bytes) {
        getrlimit(RLIMIT_FSIZE, &saved);
        saved_handler = std::signal(SIGXFSZ, SIG_IGN);
        rlimit lim = saved;
        lim.rlim_cur = bytes;
        setrlimit(RLIMIT_FSIZE, &lim);
    }
    ~FileSizeLimit() {
        setrlimit(RLIMIT_FSIZE, &saved);
        std::signal(SIGXFSZ, saved_handler);
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

void spit(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::trunc);
    out << text;
}

alpacahttp::Response route_request(alpacahttp::Router& router, const std::string& method, const std::string& path,
                                   const std::string& body = std::string()) {
    alpacahttp::Request request;
    std::ostringstream raw;
    raw << method << " " << path << " HTTP/1.1\r\nHost: localhost\r\n";
    if (!body.empty()) {
        raw << "Content-Type: application/json\r\nContent-Length: " << body.size() << "\r\n";
    }
    raw << "\r\n" << body;
    EXPECT(request.parse(raw.str()));
    return router.route(request, 1);
}

// Files in config/ whose name starts with "registered_devices.json.corrupt-".
std::vector<fs::path> corrupt_copies() {
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator("config")) {
        if (entry.path().filename().string().rfind("registered_devices.json.corrupt-", 0) == 0) {
            found.push_back(entry.path());
        }
    }
    return found;
}

// A corrupt registered_devices.json is set aside, not overwritten by the next save.
void corrupt_file_is_kept(const std::string& original, int device_number) {
    fs::remove_all("config");
    spit("config/registered_devices.json", original);
    alpacahttp::Router router(alpacahttp::test_catalog::add_schema_and_factory);
    const nlohmann::json body = {{"vendor", "zzz"}, {"deviceType", "focuser"}, {"deviceNumber", device_number}};
    const auto resp = route_request(router, "POST", "/management/v1/configuredevice", body.dump());
    EXPECT(nlohmann::json::parse(resp.body()).value("ErrorNumber", -1) == 0);
    const auto copies = corrupt_copies();
    EXPECT(copies.size() == 1);
    EXPECT(slurp(copies[0]) == original);
    // The save that followed wrote a parseable list.
    EXPECT(nlohmann::json::parse(slurp("config/registered_devices.json"), nullptr, false).is_array());
}

}  // namespace

// Optional argv[1] (path1, path2, path4) runs one case; no argument runs all.
int main(int argc, char** argv) {
    std::cout << "Testing atomic settings writes...\n";
    const std::string only = argc > 1 ? argv[1] : "";
    const auto run = [&](const char* name) { return only.empty() || only == name; };

    // PATH 1: zero-length and truncated registered_devices.json
    if (run("path1")) {
        corrupt_file_is_kept("", 9801);
        corrupt_file_is_kept(R"([{"vendor": "zzz", "deviceType": "foc)", 9802);
    }

    // PATH 2: a failed config.yaml rewrite leaves it byte-identical and is reported
    if (run("path2")) {
        const std::string config_path = (fs::current_path() / "config_fail.yaml").string();
        const std::string original =
            "server:\n  location: \"Opera House\"\n  profile_name: \"Sydney\"\nhttp:\n  port: \"11111\"\n";
        spit(config_path, original);
        fs::remove_all("config");
        alpacahttp::Router router;
        router.set_config_path(config_path);
        alpacahttp::Response put;
        {
            FileSizeLimit limit(16);
            put = route_request(router, "PUT", "/management/v1/description", R"({"Location":"Greenwich"})");
        }
        EXPECT(slurp(config_path) == original);
        EXPECT(nlohmann::json::parse(put.body()).value("ErrorNumber", 0) != 0);
        EXPECT(!fs::exists(config_path + ".tmp"));
        fs::remove(config_path);
    }

    // PATH 4: a failed runtime_state.json write leaves it unchanged
    if (run("path4")) {
        const std::string original = "{\"LogLevel\": \"INFO\"}";
        spit("config/runtime_state.json", original);
        {
            FileSizeLimit limit(8);
            alpacahttp::util::save_runtime_log_level(alpacacore::logging::LogLevel::Debug);
        }
        EXPECT(slurp("config/runtime_state.json") == original);
        EXPECT(!fs::exists("config/runtime_state.json.tmp"));
    }

    std::cout << "OK\n";
    return 0;
}

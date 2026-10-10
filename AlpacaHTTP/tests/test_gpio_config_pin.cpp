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

// open-astro#765: a device-config request must not make the server request an
// arbitrary GPIO line, open an arbitrary chip node or open an arbitrary device path.
//
// No hardware: libgpiod v2 is replaced by recording stubs defined in this
// executable (they take precedence over the shared library at link time), so the
// real Router config path and the real ZWO ASIAIR switch driver run unmodified.
// The ASIAIR Pro/CM4 board wires its four DC ports to GPIO 12, 13, 26, 18 only
// (.github/instructions/zwo.instructions.md).
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>
#include <alpacahttp/router.h>
#include <gpiod.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "test_assert.h"

// ---- recording libgpiod v2 stubs -----------------------------------------------------
struct gpiod_chip {
    std::string path;
};
struct gpiod_line_settings {
    int dir = 0;
    int val = 0;
};
struct gpiod_line_config {
    std::vector<unsigned> offsets;
    int dir = 0;
    int val = 0;
};
struct gpiod_request_config {
    int unused = 0;
};
struct gpiod_line_request {
    std::string chip;
};

struct GpioLog {
    std::mutex m;
    std::vector<std::string> chip_opens;
    std::vector<std::string> output_requests;  // "chip=<path> line=<n> dir=output value=<v>"
};
static GpioLog g_log;

extern "C" {
gpiod_chip* gpiod_chip_open(const char* path) {
    std::lock_guard<std::mutex> l(g_log.m);
    g_log.chip_opens.push_back(path);
    return new gpiod_chip{path};
}
void gpiod_chip_close(gpiod_chip* c) { delete c; }
gpiod_line_settings* gpiod_line_settings_new(void) { return new gpiod_line_settings; }
void gpiod_line_settings_free(gpiod_line_settings* s) { delete s; }
int gpiod_line_settings_set_direction(gpiod_line_settings* s, enum gpiod_line_direction d) {
    s->dir = static_cast<int>(d);
    return 0;
}
int gpiod_line_settings_set_output_value(gpiod_line_settings* s, enum gpiod_line_value v) {
    s->val = static_cast<int>(v);
    return 0;
}
gpiod_line_config* gpiod_line_config_new(void) { return new gpiod_line_config; }
void gpiod_line_config_free(gpiod_line_config* c) { delete c; }
int gpiod_line_config_add_line_settings(gpiod_line_config* c, const unsigned* offs, size_t n, gpiod_line_settings* s) {
    for (size_t i = 0; i < n; ++i) c->offsets.push_back(offs[i]);
    c->dir = s->dir;
    c->val = s->val;
    return 0;
}
int gpiod_line_config_set_output_values(gpiod_line_config*, const enum gpiod_line_value*, size_t) { return 0; }
gpiod_request_config* gpiod_request_config_new(void) { return new gpiod_request_config; }
void gpiod_request_config_free(gpiod_request_config* c) { delete c; }
void gpiod_request_config_set_consumer(gpiod_request_config*, const char*) {}
gpiod_line_request* gpiod_chip_request_lines(gpiod_chip* chip, gpiod_request_config*, gpiod_line_config* lc) {
    std::lock_guard<std::mutex> l(g_log.m);
    for (unsigned off : lc->offsets) {
        std::ostringstream o;
        o << "chip=" << chip->path << " line=" << off
          << " dir=" << (lc->dir == GPIOD_LINE_DIRECTION_OUTPUT ? "output" : "other")
          << " value=" << (lc->val == GPIOD_LINE_VALUE_ACTIVE ? "HIGH" : "LOW");
        g_log.output_requests.push_back(o.str());
    }
    return new gpiod_line_request{chip->path};
}
void gpiod_line_request_release(gpiod_line_request* r) { delete r; }
int gpiod_line_request_set_value(gpiod_line_request*, unsigned, enum gpiod_line_value) { return 0; }
}
// --------------------------------------------------------------------------------------

namespace {
alpacahttp::Response route(alpacahttp::Router& router, const std::string& method, const std::string& path,
                           const std::string& body) {
    alpacahttp::Request request;
    std::ostringstream raw;
    raw << method << " " << path << " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!body.empty()) {
        raw << "Content-Type: " << (body[0] == '{' ? "application/json" : "application/x-www-form-urlencoded")
            << "\r\nContent-Length: " << body.size() << "\r\n";
    }
    raw << "\r\n" << body;
    EXPECT(request.parse(raw.str()));
    return router.route(request, 1);
}

std::string configure(alpacahttp::Router& router, const std::string& json, int& status) {
    auto r = route(router, "POST", "/management/v1/configuredevice", json);
    status = r.status_code();
    return r.body();
}

bool registered(alpacahttp::Router& router, int number) {
    auto r = route(router, "GET", "/management/v1/configureddevices", "");
    return r.body().find("\"DeviceNumber\":" + std::to_string(number)) != std::string::npos;
}

std::string switch_config(int number, const std::string& type, const std::string& extra) {
    return R"({"vendor":"zwo","deviceType":"switch","deviceNumber":)" + std::to_string(number) + R"(,"switchType":")" +
           type + "\"," + extra + "}";
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    // Router persists config/registered_devices.json relative to the cwd: work in a per-run
    // subdirectory of the CMake-provided cwd so a developer's own config/ is never touched.
    const fs::path cwd = fs::current_path() / "gpio_config_pin_cwd";
    fs::remove_all(cwd);
    fs::create_directories(cwd);
    fs::current_path(cwd);

    alpacahttp::Router router;
    int status = 0;
    (void)status;

#ifdef ALPACACORE_ENABLE_ZWO
    // A line that is not an ASIAIR port, on a foreign chip node: refused with 400, not saved.
    std::string body = configure(
        router, switch_config(9701, "asiair", R"("gpioChip":"/dev/gpiochip4","ports":[{"gpio":17,"name":"Relay"}])"),
        status);
    EXPECT(status == 400);
    EXPECT(body.find("Hardware config refused") != std::string::npos);
    EXPECT(!registered(router, 9701));

    // The board's chip with a foreign line: refused.
    configure(router, switch_config(9702, "asiair-plus-picm4", R"("ports":[{"gpio":17}])"), status);
    EXPECT(status == 400);
    EXPECT(!registered(router, 9702));

    // A foreign chip with the board's lines: refused.
    configure(router, switch_config(9703, "asiair", R"("gpioChip":"/dev/gpiochip4","ports":[{"gpio":12}])"), status);
    EXPECT(status == 400);
    EXPECT(!registered(router, 9703));

    // The same line twice is refused.
    body = configure(router, switch_config(9711, "asiair", R"("ports":[{"gpio":12},{"gpio":12}])"), status);
    EXPECT(status == 400);
    EXPECT(body.find("Hardware config refused") != std::string::npos);
    EXPECT(!registered(router, 9711));

    // An integral JSON float names the same line as the integer; a value that only equals a
    // board line modulo 2^32 is a different line and is refused, not wrapped to 12.
    configure(router, switch_config(9712, "asiair", R"("ports":[{"gpio":12.0}])"), status);
    EXPECT(status == 200);
    EXPECT(registered(router, 9712));
    configure(router, switch_config(9713, "asiair", R"("ports":[{"gpio":4294967308}])"), status);
    EXPECT(status == 400);
    EXPECT(!registered(router, 9713));

    // RK3568 ports: a null entry becomes an empty port, so [null, {name:A}] is two ports and
    // the named one is DC2.
    configure(router, switch_config(9714, "asiair-plus-rk3568", R"("ports":[null,{"name":"A"}])"), status);
    EXPECT(status == 200);
    EXPECT(registered(router, 9714));
    {
        // case: RK3568 null port entry keeps its position
        const auto max_json = nlohmann::json::parse(route(router, "GET", "/api/v1/switch/9714/maxswitch", "").body());
        EXPECT(max_json.value("ErrorNumber", -1) == 0);
        EXPECT(max_json.value("Value", -1) == 2);
        const auto name_json =
            nlohmann::json::parse(route(router, "GET", "/api/v1/switch/9714/getswitchname?Id=1", "").body());
        EXPECT(name_json.value("Value", "") == "A");
    }

    // A request-supplied devicePath is refused, whatever the path.
    for (const char* path : {"/etc/hostname", "/dev/sda", "/dev/gpiochip0"}) {
        configure(router,
                  switch_config(9704, "asiair-plus-rk3568",
                                std::string(R"("devicePath":")") + path + R"(","ports":[{"name":"DC1"}])"),
                  status);
        EXPECT(status == 400);
        EXPECT(!registered(router, 9704));
    }

    // The board's own values still register: Pro defaults, all four port lines, Plus device node.
    configure(router,
              switch_config(9705, "asiair",
                            R"("gpioChip":"/dev/gpiochip0","ports":[{"gpio":12},{"gpio":13},{"gpio":26},{"gpio":18}])"),
              status);
    EXPECT(status == 200);
    EXPECT(registered(router, 9705));
    configure(router, switch_config(9706, "asiair-plus-rk3568", R"("devicePath":"/dev/pwm-gpio-misc")"), status);
    EXPECT(status == 200);
    EXPECT(registered(router, 9706));

    // A refused request never reaches the driver: connecting the one that registered requests
    // only the board's lines on the board's chip.
    route(router, "PUT", "/api/v1/switch/9705/connected", "Connected=true");
    for (auto& c : g_log.chip_opens) EXPECT(c == "/dev/gpiochip0");
    for (auto& r : g_log.output_requests) {
        EXPECT(r.find("line=12 ") != std::string::npos || r.find("line=13 ") != std::string::npos ||
               r.find("line=26 ") != std::string::npos || r.find("line=18 ") != std::string::npos);
    }
    route(router, "PUT", "/api/v1/switch/9705/connected", "Connected=false");

#endif

#if defined(ALPACACORE_ENABLE_IOPTRON) && defined(ALPACACORE_IOPTRON_POWERBOX)
    configure(router, R"({"vendor":"ioptron","deviceType":"switch","deviceNumber":9707,"gpioChip":"/dev/gpiochip4"})",
              status);
    EXPECT(status == 400);
    EXPECT(!registered(router, 9707));
    configure(router, R"({"vendor":"ioptron","deviceType":"switch","deviceNumber":9708,"gpioChip":"/dev/gpiochip0"})",
              status);
    EXPECT(status == 200);
    EXPECT(registered(router, 9708));
#endif

#if defined(ALPACACORE_ENABLE_TOUPTEK) && defined(ALPACACORE_TOUPTEK_STELLAVITA)
    configure(router,
              R"({"vendor":"touptek","deviceType":"switch","deviceNumber":9709,"switchType":"stellavita",)"
              R"("gpioChip":"/dev/gpiochip4"})",
              status);
    EXPECT(status == 400);
    EXPECT(!registered(router, 9709));
    configure(router,
              R"({"vendor":"touptek","deviceType":"switch","deviceNumber":9710,"switchType":"stellavita",)"
              R"("gpioChip":"/dev/gpiochip0"})",
              status);
    EXPECT(status == 200);
    EXPECT(registered(router, 9710));
#endif

    fs::current_path(cwd.parent_path());
    fs::remove_all(cwd);
    return 0;
}

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

// Log level of the router's line for an exception a driver throws to a client
// (open-astro#1333).

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/device_registry.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/util/logging.h>
#include <alpacahttp/request.h>
#include <alpacahttp/router.h>

#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "test_assert.h"

namespace {

// A disconnected focuser: the position getter refuses with NotConnected, as a
// real driver does.
class DisconnectedFocuser final : public alpacacore::FocuserDriver {
public:
    explicit DisconnectedFocuser(int number) : number_(number) {}
    int get_device_number() const override { return number_; }
    std::string get_name() const override { return "Log Level Focuser"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Focuser; }
    std::string fallback_unique_id() const override { return "log-level-focuser"; }
    std::string get_description() const override { return "fake device"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 1; }
    bool get_connected() const override { return connected_; }
    void set_connected(bool connected) override { connected_ = connected; }
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return {}; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return {}; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return {}; }

    bool get_absolute() const override { return true; }
    bool get_is_moving() const override { return false; }
    int get_max_step() const override { return 1000; }
    int get_max_increment() const override { return 1000; }
    int get_position() const override {
        throw alpacacore::AlpacaException("Focuser not connected", alpacacore::AlpacaError::NotConnected);
    }
    double get_step_size() const override { return 1.0; }
    bool get_temp_comp_available() const override { return false; }
    bool get_temp_comp() const override { return false; }
    void set_temp_comp(bool) override {}
    double get_temperature() const override { return 0.0; }
    void halt() override {}
    void move(int) override {}

private:
    int number_;
    bool connected_ = false;
};

alpacahttp::Response route_get(alpacahttp::Router& router, const std::string& path) {
    alpacahttp::Request request;
    std::ostringstream raw;
    raw << "GET " << path << " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    EXPECT(request.parse(raw.str()));
    return router.route(request, 1);
}

}  // namespace

int main() {
    std::cout << "Testing router log levels...\n";
    alpacahttp::Router router;

    {
        // case: NotConnected reply is logged below ERROR and the 1031 reply is unchanged (#1333)
        constexpr int kNumber = 9333;
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        EXPECT(registry.register_device(std::make_shared<DisconnectedFocuser>(kNumber)));

        struct CapturedLine {
            alpacacore::logging::LogLevel level;
            std::string message;
        };
        std::vector<CapturedLine> captured;
        std::mutex captured_mutex;
        struct LoggingRestore {
            alpacacore::logging::LogLevel level = alpacacore::logging::get_log_level();
            alpacacore::logging::LogSink sink = alpacacore::logging::get_log_sink();
            ~LoggingRestore() {
                alpacacore::logging::set_log_sink(sink);
                alpacacore::logging::set_log_level(level);
            }
        } logging_restore;
        alpacacore::logging::set_log_level(alpacacore::logging::LogLevel::Debug);
        alpacacore::logging::set_log_sink(
            [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                std::lock_guard<std::mutex> lock(captured_mutex);
                captured.push_back({level, std::string(message)});
            });

        const auto resp = route_get(router, "/api/v1/focuser/" + std::to_string(kNumber) + "/position");
        EXPECT(resp.status_code() == 200);
        const auto body = nlohmann::json::parse(resp.body());
        EXPECT(body.value("ErrorNumber", 0) == 1031);
        EXPECT(body.value("ErrorMessage", "") == "Focuser not connected");

        const std::string line = "AlpacaException in focuser method 'position': Focuser not connected";
        {
            std::lock_guard<std::mutex> lock(captured_mutex);
            int matched = 0;
            for (const auto& c : captured) {
                EXPECT(c.level != alpacacore::logging::LogLevel::Error);
                if (c.message == line) {
                    ++matched;
                    EXPECT(c.level == alpacacore::logging::LogLevel::Debug);
                }
            }
            EXPECT(matched == 1);
        }

        alpacacore::management::DeviceRegistry::instance().unregister_device(alpacacore::DeviceType::Focuser, kNumber);
    }

    std::cout << "Router log level tests passed\n";
    return 0;
}

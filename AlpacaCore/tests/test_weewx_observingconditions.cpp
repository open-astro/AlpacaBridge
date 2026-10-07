// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>
#include <alpacacore/version.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "catch2_compat.h"

namespace {

// Loopback HTTP server standing in for the WeeWX JSON feed: answers every
// request with the current body, then closes the connection.
class FakeWeeWxFeed {
public:
    FakeWeeWxFeed() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd_ >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
        REQUIRE(::listen(fd_, 8) == 0);
        thread_ = std::thread([this] { run(); });
    }

    ~FakeWeeWxFeed() {
        stop_ = true;
        thread_.join();
        ::close(fd_);
    }

    FakeWeeWxFeed(const FakeWeeWxFeed&) = delete;
    FakeWeeWxFeed& operator=(const FakeWeeWxFeed&) = delete;

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/current"; }

    void set_body(std::string body) {
        std::lock_guard<std::mutex> lock(mutex_);
        body_ = std::move(body);
    }

private:
    void run() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) {
                continue;
            }
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            char buf[2048];
            (void)::recv(client, buf, sizeof(buf), 0);
            std::string body;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                body = body_;
            }
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
            ::close(client);
        }
    }

    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::string body_;
};

// Temperature and humidity only, observed now.
std::string weewx_payload() {
    const long long now = static_cast<long long>(std::time(nullptr));
    return R"({"lcd_datasheet":{"current":{"outTemp":{"value":50.0},"outHumidity":{"value":40.0},"dateTime":{"value":)" +
           std::to_string(now) + "}}}}";
}

int alpaca_error_of(const std::function<void()>& fn) {
    try {
        fn();
        return 0;
    } catch (const alpacacore::AlpacaException& ex) {
        return ex.error_code();
    }
}

}  // namespace

TEST_CASE("WeeWX current parsing", "[weewx]") {
    const std::string payload = R"json(
{
    "lcd_datasheet": {
        "current": {
            "outTemp": {"value": 50.0, "units": "\u00b0F"},
            "outHumidity": {"value": 40.0, "units": "%"},
            "dewpoint": {"value": 32.0, "units": "\u00b0F"},
            "wind_speed": {"value": 10.0, "units": "mph"},
            "barometer": {"value": 30.0, "units": "inHg"},
            "sqm": {"value": 21.3},
            "sqmTemp": {"value": 41.0}
        },
        "daily_captures": {
            "rows": [
                [1, 2, , 4]
            ]
        }
    }
}
)json";

    auto values = alpacacore::vendor::weewx::parse_weewx_current(payload);
    REQUIRE(values.has_value());

    const auto& v = values.value();
    REQUIRE(std::abs(v.temperature_c - 10.0) < 1e-6);
    REQUIRE(std::abs(v.humidity - 40.0) < 1e-6);
    REQUIRE(std::abs(v.dewpoint_c - 0.0) < 1e-6);
    REQUIRE(std::abs(v.wind_speed_ms - 4.4704) < 1e-4);
    REQUIRE(std::abs(v.pressure_hpa - 1015.9166) < 1e-3);
    REQUIRE(std::abs(v.sky_quality - 21.3) < 1e-6);
    REQUIRE(std::abs(v.sky_temperature_c - 5.0) < 1e-6);
}

TEST_CASE("WeeWX ObservingConditions Driver - Defaults", "[weewx]") {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";

    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(7, config);
    REQUIRE(driver);

    CHECK(driver->get_device_type() == alpacacore::DeviceType::ObservingConditions);
    CHECK(driver->get_device_number() == 7);
    CHECK(driver->get_connected() == false);
    CHECK(driver->get_name() == "WeeWX ObservingConditions");
}

TEST_CASE("WeeWX ObservingConditions Driver - Device metadata", "[weewx]") {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";

    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(3, config);
    REQUIRE(driver);

    CHECK(driver->get_description() == "WeeWX ObservingConditions from HTTP JSON");
    CHECK(driver->get_driver_info() == "AlpacaCore WeeWX ObservingConditions Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 2);
    CHECK(driver->get_unique_id() == "WEEWX_OC_3");
}

TEST_CASE("WeeWX ObservingConditions Driver - Unsupported actions", "[weewx]") {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";

    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config);
    REQUIRE(driver);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("foo", "bar"), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("foo", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("foo", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("foo", false), alpacacore::AlpacaException);
}

TEST_CASE("WeeWX ObservingConditions Driver - ASCOM Error Codes", "[weewx]") {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";

    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config);
    REQUIRE(driver);

    auto require_alpaca_error = [](const std::function<void()>& fn, int expected_code) {
        try {
            fn();
            FAIL("Expected AlpacaException");
        } catch (const alpacacore::AlpacaException& ex) {
            REQUIRE(ex.error_code() == expected_code);
        }
    };

    require_alpaca_error([&]() { (void)driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_humidity(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_dew_point(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_pressure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_wind_speed(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("WeeWX ObservingConditions Driver - AveragePeriod is 0 for instantaneous readings", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_body(weewx_payload());
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = feed.url();
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    CHECK(driver->get_average_period() == 0.0);
    CHECK(alpaca_error_of([&] { driver->set_average_period(0.0); }) == 0);
    CHECK(alpaca_error_of([&] { driver->set_average_period(1.0); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(alpaca_error_of([&] { driver->set_average_period(-1.0); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(driver->get_average_period() == 0.0);
    driver->set_connected(false);
}

TEST_CASE("WeeWX ObservingConditions Driver - TimeSinceLastUpdate empty name is the latest sensor update", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_body(weewx_payload());
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = feed.url();
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    const double named = driver->get_time_since_last_update("Temperature");
    const double any = driver->get_time_since_last_update("");
    INFO("Temperature=" << named << " any=" << any);
    REQUIRE(named >= 0.0);
    REQUIRE(named < 30.0);
    CHECK(any >= 0.0);
    CHECK(any < 30.0);
    driver->set_connected(false);
}

TEST_CASE("WeeWX ObservingConditions Driver - Unknown sensor name is InvalidValue", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_body(weewx_payload());
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = feed.url();
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config);

    // Disconnected: the name is validated before anything else.
    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update("NoSuchSensor"); }) ==
          alpacacore::AlpacaError::InvalidValue);
    CHECK(alpaca_error_of([&] { (void)driver->get_sensor_description("NoSuchSensor"); }) ==
          alpacacore::AlpacaError::InvalidValue);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update("NoSuchSensor"); }) ==
          alpacacore::AlpacaError::InvalidValue);
    CHECK(alpaca_error_of([&] { (void)driver->get_sensor_description("NoSuchSensor"); }) ==
          alpacacore::AlpacaError::InvalidValue);
    // A sensor IObservingConditions defines but this station does not serve stays NotImplemented.
    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update("CloudCover"); }) ==
          alpacacore::AlpacaError::PropertyNotImplemented);
    CHECK(alpaca_error_of([&] { (void)driver->get_sensor_description("CloudCover"); }) ==
          alpacacore::AlpacaError::PropertyNotImplemented);
    CHECK(driver->get_sensor_description("Temperature") == "Outdoor temperature (WeeWX outTemp)");
    driver->set_connected(false);
}

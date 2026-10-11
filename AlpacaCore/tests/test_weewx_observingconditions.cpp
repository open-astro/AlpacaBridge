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
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"

namespace {

// Loopback HTTP server standing in for the WeeWX JSON feed: answers every
// request with the current body, then closes the connection. Modes make it
// harsher than a healthy station: Stall accepts and reads the request but never
// replies and never closes (a hung WeeWX or a dead Wi-Fi link behind an open
// socket), DropAfterAccept closes with no bytes, ServerError answers 500.
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
        for (const int held : stalled_) {
            ::close(held);
        }
        ::close(fd_);
    }

    FakeWeeWxFeed(const FakeWeeWxFeed&) = delete;
    FakeWeeWxFeed& operator=(const FakeWeeWxFeed&) = delete;

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/current"; }

    enum class Mode { Normal, Stall, DropAfterAccept, ServerError };

    void set_mode(Mode mode) { mode_ = mode; }

    // Requests accepted so far, in any mode.
    int requests() const { return requests_; }

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
            ++requests_;
            const Mode mode = mode_;
            if (mode == Mode::Stall) {
                stalled_.push_back(client);  // held open, never answered
                continue;
            }
            if (mode == Mode::DropAfterAccept) {
                ::close(client);
                continue;
            }
            if (mode == Mode::ServerError) {
                const std::string err =
                    "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                (void)::send(client, err.data(), err.size(), MSG_NOSIGNAL);
                ::close(client);
                continue;
            }
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
    std::atomic<Mode> mode_{Mode::Normal};
    std::atomic<int> requests_{0};
    std::vector<int> stalled_;  // touched only by the server thread, then the destructor
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

// Every sensor the driver can serve, observed `age_s` seconds ago.
std::string full_payload(long long age_s = 0) {
    const long long at = static_cast<long long>(std::time(nullptr)) - age_s;
    return R"({"lcd_datasheet":{"current":{"outTemp":{"value":50.0},"outHumidity":{"value":40.0},)"
           R"("dewpoint":{"value":32.0},"wind_speed":{"value":10.0},"barometer":{"value":30.0},)"
           R"("sqm":{"value":21.3},"sqmTemp":{"value":41.0},"dateTime":{"value":)" +
           std::to_string(at) + "}}}}";
}

std::unique_ptr<alpacacore::ObservingConditionsDriver> make_driver(
    const FakeWeeWxFeed& feed, std::chrono::milliseconds timeout = std::chrono::milliseconds(300),
    std::chrono::seconds poll = std::chrono::seconds(1)) {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = feed.url();
    config.timeout = timeout;
    config.poll_interval = poll;
    return alpacacore::vendor::weewx::create_weewx_observingconditions(0, config);
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

TEST_CASE("WeeWX ObservingConditions Driver - every member connected over a full feed", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_body(full_payload());
    auto driver = make_driver(feed);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    CHECK(std::abs(driver->get_temperature() - 10.0) < 1e-6);
    CHECK(std::abs(driver->get_humidity() - 40.0) < 1e-6);
    CHECK(std::abs(driver->get_dew_point() - 0.0) < 1e-6);
    CHECK(std::abs(driver->get_wind_speed() - 4.4704) < 1e-4);
    CHECK(std::abs(driver->get_pressure() - 1015.9166) < 1e-3);
    CHECK(std::abs(driver->get_sky_quality() - 21.3) < 1e-6);
    CHECK(std::abs(driver->get_sky_temperature() - 5.0) < 1e-6);

    const int not_impl = alpacacore::AlpacaError::PropertyNotImplemented;
    CHECK(alpaca_error_of([&] { (void)driver->get_cloud_cover(); }) == not_impl);
    CHECK(alpaca_error_of([&] { (void)driver->get_rain_rate(); }) == not_impl);
    CHECK(alpaca_error_of([&] { (void)driver->get_wind_direction(); }) == not_impl);
    CHECK(alpaca_error_of([&] { (void)driver->get_wind_gust(); }) == not_impl);
    CHECK(alpaca_error_of([&] { (void)driver->get_sky_brightness(); }) == not_impl);
    CHECK(alpaca_error_of([&] { (void)driver->get_star_fwhm(); }) == not_impl);
    CHECK(alpaca_error_of([&] { (void)driver->get_seeing(); }) == not_impl);

    CHECK(driver->get_sensor_description("DewPoint") == "Dew point (WeeWX dewpoint)");
    CHECK(driver->get_sensor_description("wind speed") == "Wind speed (WeeWX wind_speed)");
    CHECK(driver->get_sensor_description("SkyQuality") == "Sky quality (WeeWX sqm)");
    CHECK(driver->get_sensor_description("SkyTemperature") == "Sky sensor temperature (WeeWX sqmTemp)");
    for (const char* unserved : {"CloudCover", "RainRate", "WindDirection", "WindGust", "SkyBrightness", "StarFWHM"}) {
        INFO(unserved);
        CHECK(alpaca_error_of([&] { (void)driver->get_sensor_description(unserved); }) == not_impl);
        CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update(unserved); }) == not_impl);
    }
    driver->set_connected(false);
}

TEST_CASE("WeeWX ObservingConditions Driver - sensors the feed lacks are NotImplemented", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_body(weewx_payload());  // temperature and humidity only
    auto driver = make_driver(feed);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    CHECK(driver->get_temperature() > 0.0);
    CHECK(alpaca_error_of([&] { (void)driver->get_dew_point(); }) == alpacacore::AlpacaError::PropertyNotImplemented);
    CHECK(alpaca_error_of([&] { (void)driver->get_pressure(); }) == alpacacore::AlpacaError::PropertyNotImplemented);
    CHECK(alpaca_error_of([&] { (void)driver->get_wind_speed(); }) == alpacacore::AlpacaError::PropertyNotImplemented);
    CHECK(alpaca_error_of([&] { (void)driver->get_sky_quality(); }) == alpacacore::AlpacaError::PropertyNotImplemented);
    CHECK(alpaca_error_of([&] { (void)driver->get_sensor_description("Pressure"); }) ==
          alpacacore::AlpacaError::PropertyNotImplemented);

    // A sensor that gains data later (SQM after sunset) becomes supported via Refresh.
    feed.set_body(full_payload());
    driver->refresh();
    CHECK(std::abs(driver->get_sky_quality() - 21.3) < 1e-6);
    CHECK(driver->get_sensor_description("SkyQuality") == "Sky quality (WeeWX sqm)");

    // A supported sensor that then drops out of the feed has no value: ValueNotSet, never a stale number.
    feed.set_body(weewx_payload());
    driver->refresh();
    CHECK(alpaca_error_of([&] { (void)driver->get_sky_quality(); }) == alpacacore::AlpacaError::ValueNotSet);
    driver->set_connected(false);
}

TEST_CASE("WeeWX ObservingConditions Driver - Refresh pulls a new reading", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_body(full_payload());
    auto driver = make_driver(feed);  // 1 s poll: Refresh must not wait for it
    REQUIRE(alpaca_error_of([&] { driver->refresh(); }) == alpacacore::AlpacaError::NotConnected);
    driver->set_connected(true);
    CHECK(std::abs(driver->get_temperature() - 10.0) < 1e-6);

    feed.set_body(
        R"({"lcd_datasheet":{"current":{"outTemp":{"value":68.0},"outHumidity":{"value":55.0},"dateTime":{"value":)" +
        std::to_string(static_cast<long long>(std::time(nullptr))) + "}}}}");
    driver->refresh();
    CHECK(std::abs(driver->get_temperature() - 20.0) < 1e-6);
    CHECK(std::abs(driver->get_humidity() - 55.0) < 1e-6);
    driver->set_connected(false);
}

TEST_CASE("WeeWX ObservingConditions Driver - stale observation time is reported as age", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_body(full_payload(/*age_s=*/600));  // station frozen ten minutes ago, feed still answering
    auto driver = make_driver(feed);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const double age = driver->get_time_since_last_update("Temperature");
    INFO("age=" << age);
    CHECK(age >= 599.0);
    CHECK(age < 640.0);
    driver->set_connected(false);
}

TEST_CASE("WeeWX ObservingConditions Driver - connect refuses a feed that never answers", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_mode(FakeWeeWxFeed::Mode::Stall);
    auto driver = make_driver(feed, std::chrono::milliseconds(300));
    const auto start = std::chrono::steady_clock::now();
    CHECK(alpaca_error_of([&] { driver->set_connected(true); }) == alpacacore::AlpacaError::DriverException);
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(took < std::chrono::seconds(3));  // bounded by the configured timeout
    CHECK(driver->get_connected() == false);
    CHECK(alpaca_error_of([&] { (void)driver->get_temperature(); }) == alpacacore::AlpacaError::NotConnected);
    CHECK(feed.requests() >= 1);
}

TEST_CASE("WeeWX ObservingConditions Driver - connect refuses malformed replies", "[weewx]") {
    FakeWeeWxFeed feed;
    auto driver = make_driver(feed);
    using Mode = FakeWeeWxFeed::Mode;
    const int driver_exception = alpacacore::AlpacaError::DriverException;

    feed.set_body("<html>not json</html>");
    CHECK(alpaca_error_of([&] { driver->set_connected(true); }) == driver_exception);
    feed.set_body(R"({"lcd_datasheet":{"current":)");  // truncated
    CHECK(alpaca_error_of([&] { driver->set_connected(true); }) == driver_exception);
    feed.set_body(R"({"lcd_datasheet":{"other":{"outTemp":{"value":50.0}}}})");  // no current block
    CHECK(alpaca_error_of([&] { driver->set_connected(true); }) == driver_exception);
    feed.set_body("");
    CHECK(alpaca_error_of([&] { driver->set_connected(true); }) == driver_exception);
    feed.set_body(weewx_payload());
    feed.set_mode(Mode::ServerError);
    CHECK(alpaca_error_of([&] { driver->set_connected(true); }) == driver_exception);
    feed.set_mode(Mode::DropAfterAccept);
    CHECK(alpaca_error_of([&] { driver->set_connected(true); }) == driver_exception);
    CHECK(driver->get_connected() == false);

    // Every refusal left the driver reusable: a healthy feed connects.
    feed.set_mode(Mode::Normal);
    driver->set_connected(true);
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

TEST_CASE("WeeWX ObservingConditions Driver - feed stalls after connect: values go stale, Connected holds", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_body(full_payload());
    auto driver = make_driver(feed, std::chrono::milliseconds(300), std::chrono::seconds(1));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const double before = driver->get_time_since_last_update("Temperature");
    const double temp = driver->get_temperature();

    feed.set_mode(FakeWeeWxFeed::Mode::Stall);
    // Refresh surfaces the failure to the caller, bounded by the timeout.
    const auto start = std::chrono::steady_clock::now();
    CHECK(alpaca_error_of([&] { driver->refresh(); }) == alpacacore::AlpacaError::DriverException);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));

    // The poll thread keeps trying and failing; the last good reading is kept
    // and its age keeps growing, so a client can tell it is stale.
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    const double after = driver->get_time_since_last_update("Temperature");
    INFO("before=" << before << " after=" << after);
    CHECK(after >= before + 2.0);
    CHECK(driver->get_connected());
    CHECK(driver->get_temperature() == temp);

    // Recovery needs no reconnect: the next poll restores fresh data.
    feed.set_body(R"({"lcd_datasheet":{"current":{"outTemp":{"value":68.0},"dateTime":{"value":)" +
                  std::to_string(static_cast<long long>(std::time(nullptr))) + "}}}}");
    feed.set_mode(FakeWeeWxFeed::Mode::Normal);
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    CHECK(std::abs(driver->get_temperature() - 20.0) < 1e-6);

    // Disconnect and destruction do not wait on a stalled request beyond the timeout.
    feed.set_mode(FakeWeeWxFeed::Mode::Stall);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const auto t0 = std::chrono::steady_clock::now();
    driver->set_connected(false);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
    CHECK(driver->get_connected() == false);
}

TEST_CASE("WeeWX ObservingConditions Driver - malformed reply after connect keeps last good values", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_body(full_payload());
    auto driver = make_driver(feed);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    for (const char* bad : {"garbage", "{\"lcd_datasheet\":{\"current\":", "{}", ""}) {
        INFO("body=" << bad);
        feed.set_body(bad);
        CHECK(alpaca_error_of([&] { driver->refresh(); }) == alpacacore::AlpacaError::DriverException);
        CHECK(driver->get_connected());
        CHECK(std::abs(driver->get_temperature() - 10.0) < 1e-6);
    }
    // The background poll hits the same garbage and must not wipe or corrupt state.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(driver->get_connected());
    CHECK(std::abs(driver->get_humidity() - 40.0) < 1e-6);
    feed.set_mode(FakeWeeWxFeed::Mode::ServerError);
    CHECK(alpaca_error_of([&] { driver->refresh(); }) == alpacacore::AlpacaError::DriverException);
    CHECK(std::abs(driver->get_humidity() - 40.0) < 1e-6);
    driver->set_connected(false);
}

TEST_CASE("WeeWX ObservingConditions Driver - Value range validation", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_body(full_payload());
    auto driver = make_driver(feed);
    const int invalid = alpacacore::AlpacaError::InvalidValue;

    // InvalidValue precedes the connection check, so these hold while disconnected.
    CHECK(alpaca_error_of([&] { driver->set_average_period(0.5); }) == invalid);
    CHECK(alpaca_error_of([&] { driver->set_average_period(-0.001); }) == invalid);
    CHECK(alpaca_error_of([&] { driver->set_average_period(std::nan("")); }) == invalid);
    CHECK(alpaca_error_of([&] { driver->set_average_period(1e9); }) == invalid);
    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update("Rain"); }) == invalid);
    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update("temperature extra"); }) == invalid);
    CHECK(alpaca_error_of([&] { (void)driver->get_sensor_description(""); }) == invalid);
    CHECK(driver->get_average_period() == 0.0);

    // Name matching ignores case, spaces and punctuation, as the ASCOM names are spelled variously.
    driver->set_connected(true);
    CHECK(driver->get_time_since_last_update("WIND_SPEED") >= 0.0);
    CHECK(driver->get_time_since_last_update("Sky Quality") >= 0.0);
    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update("wind speed!"); }) == 0);
    CHECK(alpaca_error_of([&] { (void)driver->get_sensor_description("Rain"); }) == invalid);
    driver->set_connected(false);
}

TEST_CASE("WeeWX ObservingConditions Driver - State machine contracts", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_body(full_payload());
    auto driver = make_driver(feed);
    const int not_connected = alpacacore::AlpacaError::NotConnected;

    CHECK(driver->get_connected() == false);
    CHECK(driver->get_connecting() == false);
    CHECK(alpaca_error_of([&] { driver->refresh(); }) == not_connected);
    CHECK(alpaca_error_of([&] { (void)driver->get_sky_quality(); }) == not_connected);
    CHECK(alpaca_error_of([&] { (void)driver->get_sky_temperature(); }) == not_connected);
    CHECK(driver->get_device_state().empty());
    driver->set_connected(false);  // idempotent while disconnected
    CHECK(driver->get_connected() == false);

    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(driver->get_connecting() == false);
    driver->set_connected(true);  // idempotent while connected
    CHECK(driver->get_connected());
    CHECK_FALSE(driver->get_device_state().empty());

    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
    CHECK(alpaca_error_of([&] { (void)driver->get_temperature(); }) == not_connected);
    CHECK(alpaca_error_of([&] { driver->refresh(); }) == not_connected);
    CHECK(driver->get_device_state().empty());

    // Reconnect works and serves data again.
    driver->set_connected(true);
    CHECK(std::abs(driver->get_temperature() - 10.0) < 1e-6);

    // Async disconnect then async connect settle to the requested state.
    driver->disconnect();
    for (int i = 0; i < 100 && (driver->get_connecting() || driver->get_connected()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(driver->get_connected() == false);
    driver->connect();
    for (int i = 0; i < 100 && !driver->get_connected(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

TEST_CASE("WeeWX ObservingConditions Driver - async connect to a stalled feed reports the failure", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_mode(FakeWeeWxFeed::Mode::Stall);
    auto driver = make_driver(feed, std::chrono::milliseconds(300));
    driver->connect();
    for (int i = 0; i < 200 && driver->get_connecting(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(driver->get_connecting() == false);
    CHECK(driver->get_connected() == false);
    CHECK_FALSE(driver->get_last_connect_error().empty());
}

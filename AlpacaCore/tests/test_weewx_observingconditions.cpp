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
#include <alpacacore/util/link_health.h>
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
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_task_clock.h"
#include "fake_weewx_fetcher.h"

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

std::string alpaca_message_of(const std::function<void()>& fn) {
    try {
        fn();
        return {};
    } catch (const alpacacore::AlpacaException& ex) {
        return ex.what();
    }
}

// The link-loss cases (#1984) run the poll on the fake clock: each poll_once() is one poll, and the case waits until
// the woken poll has parked again or, after a loss, ended (decision 0005 rule 2).
constexpr std::chrono::milliseconds kRendezvous{5000};

std::unique_ptr<alpacacore::ObservingConditionsDriver> make_fake_driver(
    alpacacore::test::FakeWeeWxFetcher& fake, alpacacore::test::FakeTaskClock& clock,
    std::chrono::seconds poll = std::chrono::seconds(1)) {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";
    config.poll_interval = poll;
    return alpacacore::vendor::weewx::create_weewx_observingconditions(0, config, fake.fetcher(), clock);
}

void connect_parked(alpacacore::AlpacaDriver& driver, alpacacore::test::FakeTaskClock& clock) {
    REQUIRE(alpacacore::test::connect_and_park(driver, clock, kRendezvous));
    REQUIRE(driver.get_connected());
}

void poll_once(alpacacore::test::FakeTaskClock& clock, std::chrono::nanoseconds by = std::chrono::seconds(1)) {
    clock.advance(by);
    REQUIRE(clock.wait_for_woken_settled(kRendezvous));
}

// Three failed polls `period` apart: the third latches the fault. The fetcher must be failing.
void latch_fault(alpacacore::test::FakeTaskClock& clock, std::chrono::nanoseconds period = std::chrono::seconds(1)) {
    for (int i = 0; i < 3; ++i) {
        poll_once(clock, period);
    }
}

// Latches the fault, then one failed poll past the 30 s bound loses the feed (a 1 s poll interval).
void lose_feed(alpacacore::test::FakeTaskClock& clock) {
    latch_fault(clock);
    poll_once(clock, alpacacore::util::kLinkStalenessBound);
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
    alpacacore::test::require_alpaca_error([&]() { driver->action("foo", "bar"); },
                                           alpacacore::AlpacaError::ActionNotImplemented);
    alpacacore::test::require_alpaca_error([&]() { driver->command_blind("foo", false); },
                                           alpacacore::AlpacaError::MethodNotImplemented);
    alpacacore::test::require_alpaca_error([&]() { driver->command_bool("foo", false); },
                                           alpacacore::AlpacaError::MethodNotImplemented);
    alpacacore::test::require_alpaca_error([&]() { driver->command_string("foo", false); },
                                           alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("WeeWX ObservingConditions Driver - ASCOM Error Codes", "[weewx]") {
    using alpacacore::test::require_alpaca_error;
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";

    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config);
    REQUIRE(driver);

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

TEST_CASE("WeeWX ObservingConditions Driver - TimeSinceLastUpdate is NotConnected after disconnect", "[weewx]") {
    FakeWeeWxFeed feed;
    feed.set_body(weewx_payload());
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = feed.url();
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config);
    const int not_connected = alpacacore::AlpacaError::NotConnected;

    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update("Temperature"); }) == not_connected);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_time_since_last_update("Temperature") >= 0.0);
    driver->set_connected(false);

    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update("Temperature"); }) == not_connected);
    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update(""); }) == not_connected);
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

TEST_CASE("WeeWX ObservingConditions Driver - connect refuses a non-HTTP feed URL", "[weewx]") {
    // The feed URL is user-set, and libcurl accepts every protocol it was built with
    // (file://, ldap://, ...). A file:// URL holding a valid payload is the observable
    // case: it connects unless the fetch is limited to http and https.
    const auto path = std::filesystem::temp_directory_path() / "alpacabridge-weewx-scheme-test.json";
    {
        std::ofstream out(path);
        out << full_payload();
    }
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "file://" + path.string();
    config.timeout = std::chrono::milliseconds(300);
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config);
    CHECK(alpaca_error_of([&] { driver->set_connected(true); }) == alpacacore::AlpacaError::DriverException);
    CHECK(driver->get_connected() == false);
    std::filesystem::remove(path);
}

// The poll interval is waited out on the fake clock (decision 0005 rule 2): each advance() is one poll.
// Falsified by: weewx_observingconditions_driver.cpp note_poll_failure() never latching (kFailuresToLatch raised to
// 1000): the third failed poll serves the last value instead of refusing it, and Connected never drops.
TEST_CASE(
    "WeeWX ObservingConditions Driver - feed stalls after connect: the third failure latches a fault that refuses "
    "reads, the staleness bound drops Connected",
    "[weewx]") {
    alpacacore::test::FakeTaskClock clock;  // first: it and the fetcher outlive the driver
    alpacacore::test::FakeWeeWxFetcher fake;
    auto driver = make_fake_driver(fake, clock);
    connect_parked(*driver, clock);
    const double before = driver->get_time_since_last_update("Temperature");
    const double temp = driver->get_temperature();

    fake.fail_next(1000);

    // Fewer than three failed polls: the last good reading is served and its age is not reset, so a client can
    // tell it is stale.
    const int calls = fake.calls();
    poll_once(clock);
    poll_once(clock);
    CHECK(fake.calls() == calls + 2);
    const double after = driver->get_time_since_last_update("Temperature");
    INFO("before=" << before << " after=" << after);
    CHECK(after >= before);
    CHECK(driver->get_connected());
    CHECK(driver->get_temperature() == temp);
    CHECK(driver->get_link_fault().empty());
    // An explicit Refresh against the failing feed throws, keeps the last reading and does not count.
    CHECK_THROWS(driver->refresh());
    CHECK(driver->get_temperature() == temp);
    CHECK(driver->get_link_fault().empty());

    // The third failed poll latches: Connected holds, reads refuse with the fault text.
    poll_once(clock);
    CHECK(driver->get_connected());
    CHECK_FALSE(driver->get_link_fault().empty());
    CHECK(alpaca_error_of([&] { (void)driver->get_temperature(); }) == alpacacore::AlpacaError::DriverException);

    // A feed that comes back before the bound restores fresh data with no reconnect.
    fake.fail_next(0);
    poll_once(clock);
    CHECK(fake.calls() == calls + 5);
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    CHECK(std::abs(driver->get_temperature() - 10.0) < 1e-6);

    // Stalled again past the bound: the feed is lost.
    fake.fail_next(1000);
    latch_fault(clock);
    poll_once(clock, alpacacore::util::kLinkStalenessBound);
    CHECK_FALSE(driver->get_connected());
    CHECK(alpaca_error_of([&] { (void)driver->get_temperature(); }) == alpacacore::AlpacaError::NotConnected);

    // Disconnect and destruction do not wait on a stalled request beyond the request itself.
    fake.fail_next(0);
    connect_parked(*driver, clock);
    fake.set_delay(std::chrono::milliseconds(300));
    clock.advance(std::chrono::seconds(1));
    const auto t0 = std::chrono::steady_clock::now();
    driver->set_connected(false);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
    CHECK(driver->get_connected() == false);
}

// Decision 0009 rule 1 (#1984): no HTTP failure is positive evidence the feed is gone. A connection the server drops
// (a libcurl error), a 500 and a body that does not parse each count as one failed exchange, through the real
// libcurl fetch; three in a row of any kind latch, and one good fetch in between starts the count again.
// Falsified by: weewx_observingconditions_driver.cpp http_get() refusing only a status above 500 (the 500 replies
// never count, so they never latch); also note_poll_reply() not calling link_health_.on_reply() (a good fetch in
// between does not reset the count).
TEST_CASE("WeeWX ObservingConditions Driver - every kind of failed poll fetch counts as one failed exchange",
          "[weewx]") {
    alpacacore::test::FakeTaskClock clock;  // first: it and the feed outlive the driver
    FakeWeeWxFeed feed;
    feed.set_body(full_payload());
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = feed.url();
    config.timeout = std::chrono::milliseconds(300);
    config.poll_interval = std::chrono::seconds(1);
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config, clock);
    connect_parked(*driver, clock);

    using Mode = FakeWeeWxFeed::Mode;
    const auto fail_by = [&](int kind) {
        feed.set_mode(Mode::Normal);
        feed.set_body(full_payload());
        if (kind == 0) {
            feed.set_mode(Mode::DropAfterAccept);  // libcurl: "HTTP request failed: ..."
        } else if (kind == 1) {
            feed.set_mode(Mode::ServerError);  // "HTTP request returned status 500"
        } else {
            feed.set_body("<html>not json</html>");  // "WeeWX payload missing lcd_datasheet"
        }
    };
    const auto heal = [&] {
        feed.set_mode(Mode::Normal);
        feed.set_body(full_payload());
        poll_once(clock);
        REQUIRE(driver->get_link_fault().empty());
    };
    const char* const texts[] = {"HTTP request failed", "HTTP request returned status 500", "lcd_datasheet"};

    // Each kind alone: two failures are not a fault, the third is, and its text names the failure.
    for (int kind = 0; kind < 3; ++kind) {
        INFO("kind=" << kind);
        fail_by(kind);
        poll_once(clock);
        poll_once(clock);
        CHECK(driver->get_link_fault().empty());
        CHECK(std::abs(driver->get_temperature() - 10.0) < 1e-6);
        poll_once(clock);
        CHECK(driver->get_connected());
        const std::string fault = driver->get_link_fault();
        INFO("fault=" << fault);
        CHECK(fault.find(texts[kind]) != std::string::npos);
        CHECK(fault.find("(3 consecutive failures)") != std::string::npos);
        heal();
    }

    // A mix of the three latches after three in a row.
    for (int kind = 0; kind < 3; ++kind) {
        fail_by(kind);
        poll_once(clock);
    }
    CHECK(driver->get_link_fault().find("lcd_datasheet") != std::string::npos);
    heal();

    // One good fetch between two pairs of failures resets the count: four failures, no fault.
    for (int pair = 0; pair < 2; ++pair) {
        fail_by(1);
        poll_once(clock);
        poll_once(clock);
        if (pair == 0) {
            heal();
        }
    }
    CHECK(driver->get_link_fault().empty());
    CHECK(std::abs(driver->get_temperature() - 10.0) < 1e-6);
    feed.set_mode(Mode::Normal);
    driver->set_connected(false);
}

// Decision 0009 rule 1 (#1984): only the poll's fetches count towards the latch; a failed Refresh reports its own
// error to the caller, and a good one clears a standing fault like a good poll.
// Falsified by: weewx_observingconditions_driver.cpp refresh() not calling link_health_.on_reply() after a good fetch
// (the standing fault survives the good Refresh).
TEST_CASE("WeeWX ObservingConditions Driver - a failed Refresh does not count, a good one clears the fault",
          "[weewx]") {
    alpacacore::test::FakeTaskClock clock;  // first: it and the fetcher outlive the driver
    alpacacore::test::FakeWeeWxFetcher fake;
    auto driver = make_fake_driver(fake, clock);
    connect_parked(*driver, clock);

    fake.fail_next(3);
    for (int i = 0; i < 3; ++i) {
        CHECK_THROWS(driver->refresh());
    }
    CHECK(driver->get_link_fault().empty());
    CHECK(std::abs(driver->get_temperature() - 10.0) < 1e-6);
    // The count did not move either: two failed polls after them are still no fault.
    fake.fail_next(2);
    poll_once(clock);
    poll_once(clock);
    CHECK(driver->get_link_fault().empty());

    fake.fail_next(1);
    poll_once(clock);
    REQUIRE_FALSE(driver->get_link_fault().empty());
    driver->refresh();
    CHECK(driver->get_link_fault().empty());
    CHECK(std::abs(driver->get_temperature() - 10.0) < 1e-6);
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

// Decision 0009 rule 1 (#1984): the third failed poll in a row latches; while it stands Connected stays true and
// the sensor reads and TimeSinceLastUpdate throw DriverException with the fault text, static metadata still answers,
// and the first good poll clears it with no reconnect. A Connect while connected (faulted) changes nothing.
// Falsified by: weewx_observingconditions_driver.cpp get_value_or_nan() without require_not_faulted_locked() (a
// latched fault still serves the last good temperature); also set_connected(true) without its connected_ early
// return (the Connect stops the poll, fetches the failing feed and throws).
TEST_CASE("WeeWX ObservingConditions Driver - three consecutive failed polls latch a fault that refuses reads",
          "[weewx]") {
    alpacacore::test::FakeTaskClock clock;  // first: it and the fetcher outlive the driver
    alpacacore::test::FakeWeeWxFetcher fake;
    auto driver = make_fake_driver(fake, clock);
    connect_parked(*driver, clock);
    const int driver_exception = alpacacore::AlpacaError::DriverException;

    fake.fail_next(1000);
    poll_once(clock);
    poll_once(clock);
    CHECK(driver->get_link_fault().empty());
    CHECK(std::abs(driver->get_temperature() - 10.0) < 1e-6);
    CHECK(driver->get_time_since_last_update("Temperature") >= 0.0);

    poll_once(clock);
    CHECK(driver->get_connected());
    const std::string fault = driver->get_link_fault();
    CHECK(fault == "scripted WeeWX fetch failure (3 consecutive failures)");
    CHECK(alpaca_error_of([&] { (void)driver->get_temperature(); }) == driver_exception);
    CHECK(alpaca_error_of([&] { (void)driver->get_humidity(); }) == driver_exception);
    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update("Temperature"); }) == driver_exception);
    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update(""); }) == driver_exception);
    CHECK(alpaca_message_of([&] { (void)driver->get_temperature(); }).find(fault) != std::string::npos);
    CHECK(alpaca_message_of([&] { (void)driver->get_time_since_last_update(""); }).find(fault) != std::string::npos);
    // The parameter checks and the sensors the station does not serve keep their own answers.
    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update("NoSuchSensor"); }) ==
          alpacacore::AlpacaError::InvalidValue);
    CHECK(alpaca_error_of([&] { (void)driver->get_pressure(); }) == alpacacore::AlpacaError::PropertyNotImplemented);
    CHECK(driver->get_sensor_description("Temperature") == "Outdoor temperature (WeeWX outTemp)");
    CHECK(driver->get_device_state().empty());  // every operational read refuses (the ASCOM read-all FAQ)

    // Connect while connected is a no-op: it neither fetches nor stops the poll, and the fault stands.
    const int calls = fake.calls();
    CHECK_NOTHROW(driver->set_connected(true));
    CHECK(fake.calls() == calls);
    CHECK(driver->get_link_fault() == fault);
    REQUIRE(clock.waiter_count() == 1);  // the poll is still parked on its interval

    fake.fail_next(0);
    poll_once(clock);
    CHECK(fake.calls() == calls + 1);
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    CHECK(std::abs(driver->get_temperature() - 10.0) < 1e-6);
    CHECK_FALSE(driver->get_device_state().empty());
    driver->set_connected(false);
}

// Decision 0009 rules 2 and 3 (#1984): with a 1 s poll the bound is util::kLinkStalenessBound, measured on the task
// clock from the latch. Failures alone never lose the feed: 29 s of failed polls after the latch leave it connected;
// the poll exactly 30 s after the latch loses it.
// Falsified by: weewx_observingconditions_driver.cpp note_poll_failure() losing the feed on a failure count
// (link_health_.consecutive_failures() >= 10) instead of link_health_.fault_stale().
TEST_CASE("WeeWX ObservingConditions Driver - the feed is lost only at the staleness bound", "[weewx]") {
    alpacacore::test::FakeTaskClock clock;  // first: it and the fetcher outlive the driver
    alpacacore::test::FakeWeeWxFetcher fake;
    auto driver = make_fake_driver(fake, clock);
    connect_parked(*driver, clock);

    fake.fail_next(1000);
    latch_fault(clock);
    for (int i = 0; i < 29; ++i) {
        poll_once(clock);
    }
    CHECK(driver->get_connected());
    const int calls = fake.calls();
    clock.advance(std::chrono::milliseconds(999));  // 29.999 s after the latch: no poll is due
    CHECK(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    CHECK(fake.calls() == calls);
    CHECK(driver->get_connected());
    poll_once(clock, std::chrono::milliseconds(1));  // the poll 30 s after the latch
    CHECK(fake.calls() == calls + 1);
    CHECK_FALSE(driver->get_connected());
}

// Decision 0009 point 2(b) (#1984): the bound is max(30 s, 2 x poll_interval), so a slowly polled feed is not lost at
// its first failed poll after the latch.
// Falsified by: weewx_observingconditions_driver.cpp loss_bound() returning util::kLinkStalenessBound alone (a 20 s
// poll loses the feed 39.999 s after the latch).
TEST_CASE("WeeWX ObservingConditions Driver - the staleness bound grows with the poll interval", "[weewx]") {
    alpacacore::test::FakeTaskClock clock;  // first: it and the fetcher outlive the driver
    alpacacore::test::FakeWeeWxFetcher fake;
    auto driver = make_fake_driver(fake, clock, std::chrono::seconds(20));
    connect_parked(*driver, clock);

    fake.fail_next(1000);
    latch_fault(clock, std::chrono::seconds(20));
    poll_once(clock, std::chrono::milliseconds(39999));  // one late poll, 39.999 s after the latch
    CHECK(driver->get_connected());
    CHECK_FALSE(driver->get_link_fault().empty());
    poll_once(clock, std::chrono::seconds(20));  // 59.999 s: past the 40 s bound
    CHECK_FALSE(driver->get_connected());
}

// Decision 0009 rules 3 and 4 (#1984): a lost feed reads Connected false, every member throws NotConnected (Refresh
// and TimeSinceLastUpdate included, with the kept fault in the message), DeviceState is empty, and the poll has ended:
// no fetch follows however far the clock moves.
// Falsified by: weewx_observingconditions_driver.cpp poll_loop() going on after note_poll_failure() returns true
// (the poll keeps fetching after the loss).
TEST_CASE("WeeWX ObservingConditions Driver - a lost feed drops Connected and its poll ends", "[weewx]") {
    alpacacore::test::FakeTaskClock clock;  // first: it and the fetcher outlive the driver
    alpacacore::test::FakeWeeWxFetcher fake;
    auto driver = make_fake_driver(fake, clock);
    connect_parked(*driver, clock);
    const int not_connected = alpacacore::AlpacaError::NotConnected;

    fake.fail_next(1000);
    lose_feed(clock);
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_connecting());
    const std::string fault = driver->get_link_fault();
    CHECK(fault.find("scripted WeeWX fetch failure") != std::string::npos);
    CHECK(alpaca_error_of([&] { (void)driver->get_temperature(); }) == not_connected);
    CHECK(alpaca_error_of([&] { (void)driver->get_humidity(); }) == not_connected);
    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update("Temperature"); }) == not_connected);
    CHECK(alpaca_error_of([&] { (void)driver->get_time_since_last_update(""); }) == not_connected);
    CHECK(alpaca_error_of([&] { (void)driver->get_sensor_description("Temperature"); }) == not_connected);
    CHECK(alpaca_error_of([&] { (void)driver->get_cloud_cover(); }) == not_connected);
    CHECK(alpaca_message_of([&] { (void)driver->get_temperature(); }).find(fault) != std::string::npos);
    const int calls = fake.calls();
    CHECK(alpaca_error_of([&] { driver->refresh(); }) == not_connected);
    CHECK(driver->get_device_state().empty());

    CHECK(clock.waiter_count() == 0);  // the poll body has returned
    for (int i = 0; i < 5; ++i) {
        clock.advance(std::chrono::seconds(60));
    }
    CHECK(fake.calls() == calls);  // Refresh did not fetch, and no poll did
    CHECK_FALSE(driver->get_connected());
}

// Decision 0009 rule 4 (#1984): nothing reopens a lost feed but the client's Connect, which re-runs the connect fetch:
// it fails with DriverException while the feed is down and reconnects with a fresh value once it answers. A
// Disconnect over a lost feed completes, through either entry point.
// Falsified by: weewx_observingconditions_driver.cpp set_connected(true) without link_health_.reset() (the failed
// Connect over the lost feed still reports the old fault text).
TEST_CASE("WeeWX ObservingConditions Driver - only the client's Connect relinks a lost feed", "[weewx]") {
    alpacacore::test::FakeTaskClock clock;  // first: it and the fetcher outlive the driver
    alpacacore::test::FakeWeeWxFetcher fake;
    auto driver = make_fake_driver(fake, clock);
    connect_parked(*driver, clock);

    fake.fail_next(1000);
    lose_feed(clock);
    REQUIRE_FALSE(driver->get_connected());

    // The feed is still down: Connect re-runs the connect fetch, fails with its error (the scripted fetcher throws a
    // plain std::runtime_error; libcurl's fetch throws DriverException, "connect refuses a feed that never answers")
    // and leaves the driver down.
    const int before_connect = fake.calls();
    CHECK_THROWS_WITH(driver->set_connected(true), "scripted WeeWX fetch failure");
    CHECK(fake.calls() == before_connect + 1);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // rule 5: the Connect ended the kept text; its own error is the reason

    // The feed is back: Connect reconnects, serves a fresh value and polls again.
    fake.fail_next(0);
    connect_parked(*driver, clock);
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    CHECK(std::abs(driver->get_temperature() - 10.0) < 1e-6);
    const int calls = fake.calls();
    poll_once(clock);
    CHECK(fake.calls() == calls + 1);

    // Disconnect over a lost feed completes (sync), and so does the async Disconnect.
    fake.fail_next(1000);
    lose_feed(clock);
    REQUIRE_FALSE(driver->get_connected());
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());

    fake.fail_next(0);
    connect_parked(*driver, clock);
    fake.fail_next(1000);
    lose_feed(clock);
    REQUIRE_FALSE(driver->get_connected());
    driver->disconnect();
    // The async Disconnect runs on the connection thread, which waits on nothing of the fake clock (real time).
    for (int i = 0; i < 200 && driver->get_connecting(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_FALSE(driver->get_connecting());
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
}

// Decision 0009 rule 5 (#1984): get_link_fault() is the one fault text: empty on a healthy feed, the PolledLinkHealth
// text while faulted, the same text after the loss, and empty again after a Disconnect and after a good Connect.
// Falsified by: weewx_observingconditions_driver.cpp lose_feed_locked() calling link_health_.reset() (the text is gone
// once the feed is lost).
TEST_CASE("WeeWX ObservingConditions Driver - the link fault text is kept until the next Connect or Disconnect",
          "[weewx]") {
    alpacacore::test::FakeTaskClock clock;  // first: it and the fetcher outlive the driver
    alpacacore::test::FakeWeeWxFetcher fake;
    auto driver = make_fake_driver(fake, clock);
    connect_parked(*driver, clock);
    CHECK(driver->get_link_fault().empty());

    fake.fail_next(1000);
    latch_fault(clock);
    const std::string fault = driver->get_link_fault();
    CHECK(fault == "scripted WeeWX fetch failure (3 consecutive failures)");
    poll_once(clock, alpacacore::util::kLinkStalenessBound);
    REQUIRE_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault() == fault);
    clock.advance(std::chrono::minutes(10));
    CHECK(driver->get_link_fault() == fault);

    driver->set_connected(false);
    CHECK(driver->get_link_fault().empty());

    fake.fail_next(0);
    connect_parked(*driver, clock);
    fake.fail_next(1000);
    lose_feed(clock);
    REQUIRE(driver->get_link_fault() == fault);
    fake.fail_next(0);
    connect_parked(*driver, clock);
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    driver->set_connected(false);
}

// Decision 0009 rule 3 (#1984): there is no transport to close, so a loss drops the cached snapshot and the per-sensor
// update times; the next Connect starts clean. A sensor the reconnected feed no longer reports has no update time
// (TimeSinceLastUpdate -1), not the age of a reading taken before the loss.
// Falsified by: weewx_observingconditions_driver.cpp lose_feed_locked() not clearing property_last_update_ (Humidity
// keeps its pre-loss update time after the reconnect).
TEST_CASE("WeeWX ObservingConditions Driver - a lost feed drops its cached sensor times", "[weewx]") {
    alpacacore::test::FakeTaskClock clock;  // first: it and the fetcher state outlive the driver
    std::atomic<bool> failing{false};
    std::atomic<bool> humidity{true};
    alpacacore::vendor::weewx::WeeWxFetcher fetcher = [&](const std::string&, std::chrono::milliseconds) {
        if (failing.load()) {
            throw std::runtime_error("scripted WeeWX fetch failure");
        }
        const std::string now = std::to_string(static_cast<long long>(std::time(nullptr)));
        const std::string rh = humidity.load() ? R"("outHumidity":{"value":40.0},)" : "";
        return R"({"lcd_datasheet":{"current":{"outTemp":{"value":50.0},)" + rh + R"("dateTime":{"value":)" + now +
               "}}}}";
    };
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";
    config.poll_interval = std::chrono::seconds(1);
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config, fetcher, clock);
    connect_parked(*driver, clock);
    REQUIRE(driver->get_time_since_last_update("Humidity") >= 0.0);

    failing.store(true);
    lose_feed(clock);
    REQUIRE_FALSE(driver->get_connected());

    // The feed answers again, without its humidity sensor.
    humidity.store(false);
    failing.store(false);
    connect_parked(*driver, clock);
    CHECK(driver->get_time_since_last_update("Temperature") >= 0.0);
    CHECK(driver->get_time_since_last_update("Humidity") == -1.0);
    CHECK(alpaca_error_of([&] { (void)driver->get_humidity(); }) == alpacacore::AlpacaError::ValueNotSet);
    driver->set_connected(false);
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

TEST_CASE("WeeWX ObservingConditions Driver - empty fetcher is refused at construction", "[weewx]") {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";

    CHECK(alpaca_error_of([&] {
              (void)alpacacore::vendor::weewx::create_weewx_observingconditions(
                  0, config, alpacacore::vendor::weewx::WeeWxFetcher{});
          }) == alpacacore::AlpacaError::InvalidValue);
}

// Decision 0006 rule 1 (#2085): the poll runs in an operation slot on the driver's TaskClock, and a
// disconnect while it waits ends it at once.
// Falsified by: weewx_observingconditions_driver.cpp poll_loop() waiting on a wall-clock condition variable
// (advancing the fake clock makes no poll).
TEST_CASE("WeeWX ObservingConditions Driver - the poll waits on the task clock and a disconnect ends it", "[weewx]") {
    alpacacore::test::FakeTaskClock clock;  // first: it and the fetcher outlive the driver
    alpacacore::test::FakeWeeWxFetcher fake;
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";
    config.poll_interval = std::chrono::seconds(60);
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config, fake.fetcher(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the poll is waiting out its interval
    const int before = fake.calls();
    clock.advance(config.poll_interval);
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));  // the woken poll fetched and waits again
    CHECK(fake.calls() == before + 1);                               // one interval on the fake clock, one poll

    const auto t0 = std::chrono::steady_clock::now();
    driver->set_connected(false);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
    CHECK_FALSE(driver->get_connected());
}

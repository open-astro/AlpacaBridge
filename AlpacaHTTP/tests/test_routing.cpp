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

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/alpacadriver.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/device_registry.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/logging.h>
#include <alpacahttp/request.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/host_timezone.h>
#include <alpacahttp/version.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "test_assert.h"

namespace {

alpacahttp::Response route_request(alpacahttp::Router& router, const std::string& method, const std::string& path,
                                   const std::string& body = std::string(),
                                   const std::string& remote_addr = std::string()) {
    alpacahttp::Request request;
    request.set_remote_address(remote_addr);
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

// Issue #102 back-fill helper: POST a device config, then read it back from
// configureddevices. Returns the round-tripped Config object for
// (device_type, device_number), or a null json if configuration failed or the
// device is missing — callers EXPECT(!cfg.is_null()) first, then assert every
// persisted field survived (the automated catch for sanitize_device_config
// allowlist gaps and FormData-style key loss).
nlohmann::json roundtrip_config(alpacahttp::Router& router, const nlohmann::json& configure_body,
                                const std::string& device_type, int device_number) {
    const auto configure_response =
        route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
    // Non-JSON bodies (server error paths) yield a null return -- a clean
    // EXPECT diagnostic -- rather than an uncaught parse_error.
    const auto configure_json = nlohmann::json::parse(configure_response.body(), nullptr, false);
    if (configure_json.is_discarded() || configure_json.value("ErrorNumber", -1) != 0) {
        return nlohmann::json();
    }
    const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
    const auto configured_json = nlohmann::json::parse(configured_response.body(), nullptr, false);
    if (configured_json.is_discarded()) {
        return nlohmann::json();
    }
    // An error envelope has no "Value"; return null for a clean EXPECT
    // diagnostic instead of an uncaught json::out_of_range on the const
    // subscript below.
    if (!configured_json.contains("Value") || !configured_json["Value"].is_array()) {
        return nlohmann::json();
    }
    for (const auto& entry : configured_json["Value"]) {
        if (entry.value("DeviceType", "") == device_type && entry.value("DeviceNumber", -1) == device_number) {
            return entry.value("Config", nlohmann::json());
        }
    }
    return nlohmann::json();
}

void remove_device(alpacahttp::Router& router, const std::string& vendor, const std::string& device_type,
                   int device_number) {
    nlohmann::json body = {{"vendor", vendor}, {"deviceType", device_type}, {"deviceNumber", device_number}};
    const auto response = route_request(router, "POST", "/management/v1/removedevice", body.dump());
    const auto json = nlohmann::json::parse(response.body(), nullptr, false);
    // A failed cleanup leaves the device registered and poisons later blocks
    // that reuse the number or read configureddevices -- fail HERE instead.
    EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
}

// Issue open-astro/AlpacaBridge#647 helpers. roundtrip_config() above POSTs
// /management/v1/configuredevice, which registers with ConfigSource::Api. The
// only caller of ConfigSource::Persisted is Router::load_persisted_devices().
// It is reached from the constructor and re-entered from a few request paths,
// but the persisted_devices_loaded_ latch makes every later call return early,
// so a config only reaches the persisted path when a NEW Router is constructed
// over a file that already holds it. These helpers do exactly that.

// POST a device config through the API and report what happened. On success
// `config` is the Config object configureddevices shows for the device.
struct ApiAttempt {
    bool ok = false;
    std::string message;
    nlohmann::json config;
};

// The whole configureddevices row for a device, matched on `device_type`
// exactly and case-sensitively ("Telescope"). A persisted entry that failed to
// register is listed too, but with a LOWER-CASE DeviceType, so it does not match
// here: null means "not registered", not "not listed". Use listed_failed_entry()
// for the failed row.
nlohmann::json listed_entry(alpacahttp::Router& router, const std::string& device_type, int device_number) {
    const auto listed =
        nlohmann::json::parse(route_request(router, "GET", "/management/v1/configureddevices").body(), nullptr, false);
    if (listed.is_discarded() || !listed.contains("Value") || !listed["Value"].is_array()) {
        return nlohmann::json();
    }
    for (const auto& entry : listed["Value"]) {
        if (entry.value("DeviceType", "") == device_type && entry.value("DeviceNumber", -1) == device_number) {
            return entry;
        }
    }
    return nlohmann::json();
}

nlohmann::json listed_config(alpacahttp::Router& router, const std::string& device_type, int device_number) {
    const auto entry = listed_entry(router, device_type, device_number);
    return entry.is_null() ? nlohmann::json() : entry.value("Config", nlohmann::json());
}

// The "<vendor> (failed to load)" row configureddevices lists for a persisted
// entry that did not register (LoadError true, lower-case DeviceType).
nlohmann::json listed_failed_entry(alpacahttp::Router& router, const std::string& lower_type, int device_number) {
    const auto entry = listed_entry(router, lower_type, device_number);
    if (!entry.is_null() && entry.value("LoadError", false)) {
        return entry;
    }
    return nlohmann::json();
}

ApiAttempt api_attempt(alpacahttp::Router& router, const nlohmann::json& posted, const std::string& device_type) {
    ApiAttempt attempt;
    const auto response = nlohmann::json::parse(
        route_request(router, "POST", "/management/v1/configuredevice", posted.dump()).body(), nullptr, false);
    if (response.is_discarded()) {
        attempt.message = "<non-JSON response>";
        return attempt;
    }
    if (response.value("ErrorNumber", -1) != 0) {
        attempt.message = response.value("ErrorMessage", "");
        return attempt;
    }
    attempt.ok = true;
    attempt.config = listed_config(router, device_type, posted.value("deviceNumber", -1));
    return attempt;
}

// What a fresh Router made of one entry in registered_devices.json.
struct PersistedAttempt {
    bool listed = false;          // REGISTERED: listed under its Alpaca type name, no LoadError
    nlohmann::json config;        // Config of the registered device
    bool failed_listed = false;   // not registered, but listed as "<vendor> (failed to load)"
    nlohmann::json failed_entry;  // that row (DeviceType lower-case, LoadError true, sanitized Config)
    std::vector<std::string> warnings;
    std::vector<std::string> errors;  // an exception out of a driver constructor is logged at ERROR, not WARN
};

// Write [entry] as the whole registered_devices.json, construct a Router (which
// loads it with ConfigSource::Persisted), read configureddevices, capture every
// WARN and ERROR the load logged, then put the file back and unregister the device from
// the process-wide DeviceRegistry. `device_type` is the listed name
// ("Telescope"); the entry carries the lower-case one.
PersistedAttempt persisted_attempt(const nlohmann::json& entry, const std::string& device_type) {
    const std::filesystem::path persisted = std::filesystem::path("config") / "registered_devices.json";
    const std::string vendor = entry.value("vendor", "");
    const std::string lower_type = entry.value("deviceType", "");
    const int device_number = entry.value("deviceNumber", -1);

    std::string original;
    if (std::filesystem::exists(persisted)) {
        std::ifstream in(persisted);
        original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // Only this entry: the Router below registers EVERY entry in the file, and
    // some vendors touch hardware eagerly.
    nlohmann::json entries = nlohmann::json::array();
    entries.push_back(entry);
    std::filesystem::create_directories(persisted.parent_path());
    {
        std::ofstream out(persisted, std::ios::trunc);
        out << entries.dump();
    }

    PersistedAttempt result;
    std::mutex warnings_mutex;
    auto previous_sink = alpacacore::logging::get_log_sink();
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            std::lock_guard<std::mutex> lock(warnings_mutex);
            // Only lines about THIS entry's load. Any other WARN/ERROR in the
            // process (a vendor SDK, another thread) is not this round trip's.
            const std::string text(message);
            if (text.find("persisted device") == std::string::npos && text.rfind("Persisted ", 0) != 0) {
                return;
            }
            if (level == alpacacore::logging::LogLevel::Warn) {
                result.warnings.emplace_back(text);
            } else if (level == alpacacore::logging::LogLevel::Error) {
                result.errors.emplace_back(text);
            }
        });
    alpacahttp::Router startup_router;
    result.config = listed_config(startup_router, device_type, device_number);
    result.listed = !result.config.is_null();
    if (!result.listed) {
        result.failed_entry = listed_failed_entry(startup_router, lower_type, device_number);
        result.failed_listed = !result.failed_entry.is_null();
    }
    alpacacore::logging::set_log_sink(previous_sink);

    // Restore before anything that can abort (EXPECT is abort(): no unwinding,
    // no scope guards). remove_device() is itself an EXPECT, and a device the
    // load dropped was never registered, so it is only called for a listed one.
    const auto restore_original = [&] {
        std::ofstream restore(persisted, std::ios::trunc);
        restore << (original.empty() ? std::string("[]") : original);
    };
    restore_original();
    if (result.listed) {
        remove_device(startup_router, vendor, lower_type, device_number);
        restore_original();  // remove_device() saved "[]" over the file (#408)
    }
    return result;
}

bool any_warning_contains(const std::vector<std::string>& warnings, const std::string& fragment) {
    return std::any_of(warnings.begin(), warnings.end(),
                       [&](const std::string& w) { return w.find(fragment) != std::string::npos; });
}

// Minimal driver used to verify the management configureddevices response
// surfaces get_device_firmware() and get_device_sdk_version() (web-UI only)
// when, and only when, the driver reports each value.
class FirmwareStubDriver final : public alpacacore::AlpacaDriver {
public:
    FirmwareStubDriver(int number, std::optional<std::string> firmware,
                       std::optional<std::string> sdk_version = std::nullopt)
        : number_(number), firmware_(std::move(firmware)), sdk_version_(std::move(sdk_version)) {}

    int get_device_number() const override { return number_; }
    std::string get_name() const override { return "Firmware Stub"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::CoverCalibrator; }
    std::string get_unique_id() const override { return "firmware-stub-" + std::to_string(number_); }
    std::string get_description() const override { return "fake device"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 1; }
    bool get_connected() const override { return true; }
    void set_connected(bool) override {}
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return ""; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return ""; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return ""; }

    std::optional<std::string> get_device_firmware() const override { return firmware_; }
    std::optional<std::string> get_device_sdk_version() const override { return sdk_version_; }

private:
    int number_;
    std::optional<std::string> firmware_;
    std::optional<std::string> sdk_version_;
};

// Telescope stub for the host-clock wiring tests (issue #302). Every
// TelescopeDriver member is a harmless default; only the UTCDate pair carries
// state, so a test can assert which time_point the router handed the driver
// and in what order relative to the clock step.
// Deliberately NOT an AsyncConnectable: these cases drive
// AlpacaDriver::connect()'s synchronous default, which is all the #289
// wiring needs, and inheriting the mixin without overriding connect(),
// disconnect() or get_connecting() would imply coverage of the async
// initiator that this block does not have. LockedSlowConnectStubDriver
// below is the stub that does exercise it.
class TelescopeClockStubDriver final : public alpacacore::TelescopeDriver {
public:
    explicit TelescopeClockStubDriver(int number) : number_(number) {}

    int get_device_number() const override { return number_; }
    std::string get_name() const override { return "Telescope Clock Stub"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Telescope; }
    std::string get_unique_id() const override { return "telescope-clock-stub-" + std::to_string(number_); }
    std::string get_description() const override { return "fake telescope"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 4; }
    bool get_connected() const override { return connected_; }
    void set_connected(bool connected) override { connected_ = connected; }
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return ""; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return ""; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return ""; }

    // The two members the #289 wiring actually drives.
    std::chrono::system_clock::time_point get_utc_date() const override { return utc_; }
    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        utc_ = utc;
        ++utc_writes;
        if (on_utc_write) {
            // Fires after utc_ and utc_writes are updated, so a test can
            // sample OTHER state (the host-clock step count) as of the moment
            // the driver was written to. It is not a pre-write hook.
            on_utc_write();
        }
    }

    alpacacore::AlignmentMode get_alignment_mode() const override { return alpacacore::AlignmentMode::GermanPolar; }
    double get_altitude() const override { return 0.0; }
    double get_aperture_diameter() const override { return 0.0; }
    void set_aperture_diameter(double) override {}
    double get_aperture_area() const override { return 0.0; }
    bool get_at_home() const override { return false; }
    bool get_at_park() const override { return false; }
    double get_azimuth() const override { return 0.0; }
    bool get_can_find_home() const override { return false; }
    bool get_can_park() const override { return false; }
    bool get_can_pulse_guide() const override { return false; }
    bool get_is_pulse_guiding() const override { return false; }
    bool get_can_set_declination_rate() const override { return false; }
    bool get_can_set_guide_rates() const override { return false; }
    bool get_can_set_park() const override { return false; }
    bool get_can_set_pier_side() const override { return false; }
    bool get_can_set_right_ascension_rate() const override { return false; }
    bool get_can_set_tracking() const override { return false; }
    bool get_can_slew_alt_az() const override { return false; }
    bool get_can_slew_alt_az_async() const override { return false; }
    bool get_can_sync_alt_az() const override { return false; }
    bool get_can_slew() const override { return false; }
    bool get_can_slew_async() const override { return false; }
    bool get_can_sync() const override { return false; }
    bool get_can_unpark() const override { return false; }
    double get_declination() const override { return 0.0; }
    double get_declination_rate() const override { return 0.0; }
    void set_declination_rate(double) override {}
    bool get_tracking() const override { return false; }
    void set_tracking(bool) override {}
    double get_focal_length() const override { return 0.0; }
    void set_focal_length(double) override {}
    alpacacore::GuideRate get_guide_rate() const override { return alpacacore::GuideRate{}; }
    void set_guide_rate(const alpacacore::GuideRate&) override {}
    double get_right_ascension() const override { return 0.0; }
    double get_right_ascension_rate() const override { return 0.0; }
    void set_right_ascension_rate(double) override {}
    int get_side_of_pier() const override { return 0; }
    void set_side_of_pier(int) override {}
    int get_destination_side_of_pier(double, double) const override { return 0; }
    alpacacore::EquatorialSystem get_equatorial_system() const override {
        return alpacacore::EquatorialSystem::Topocentric;
    }
    bool get_does_refraction() const override { return false; }
    void set_does_refraction(bool) override {}
    int get_slew_settle_time() const override { return 0; }
    void set_slew_settle_time(int) override {}
    double get_sidereal_time() const override { return 0.0; }
    double get_site_elevation() const override { return 0.0; }
    void set_site_elevation(double) override {}
    double get_site_latitude() const override { return 0.0; }
    void set_site_latitude(double) override {}
    double get_site_longitude() const override { return 0.0; }
    void set_site_longitude(double) override {}
    bool get_slewing() const override { return false; }
    double get_target_declination() const override { return 0.0; }
    void set_target_declination(double) override {}
    double get_target_right_ascension() const override { return 0.0; }
    void set_target_right_ascension(double) override {}
    int get_tracking_rate() const override { return 0; }
    void set_tracking_rate(int) override {}
    std::vector<int> get_tracking_rates() const override { return {}; }
    void find_home() override {}
    void park() override {}
    void pulse_guide(int, int) override {}
    void set_park() override {}
    void slew_to_coordinates(double, double) override {}
    void slew_to_coordinates_async(double, double) override {}
    void slew_to_target() override {}
    void slew_to_target_async() override {}
    void sync_to_coordinates(double, double) override {}
    void sync_to_target() override {}
    void unpark() override {}
    bool get_can_move_axis(int) const override { return false; }
    void move_axis(int, double) override {}
    std::pair<double, double> get_axis_rate_range(int) const override { return {0.0, 0.0}; }
    void abort_slew() override {}
    void slew_to_alt_az(double, double) override {}
    void slew_to_alt_az_async(double, double) override {}
    void sync_to_alt_az(double, double) override {}

    int utc_writes = 0;
    std::function<void()> on_utc_write;

private:
    int number_;
    bool connected_ = false;
    std::chrono::system_clock::time_point utc_{};
};

// Connectable stub for the per-client Connected refcounting tests
// (issue #160): tracks real connect/disconnect calls so the tests can assert
// the upstream link is only touched by the first client in / last client out.
class ConnectStubDriver final : public alpacacore::AlpacaDriver {
public:
    explicit ConnectStubDriver(int number) : number_(number) {}

    int get_device_number() const override { return number_; }
    std::string get_name() const override { return "Connect Stub"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::CoverCalibrator; }
    std::string get_unique_id() const override { return "connect-stub-" + std::to_string(number_); }
    std::string get_description() const override { return "fake device"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 1; }
    bool get_connected() const override { return connected_; }
    void set_connected(bool connected) override {
        if (!connected) cleanup_pending = false;
        if (connected && !connected_) {
            ++connect_count;
        } else if (!connected && connected_) {
            ++disconnect_count;
        }
        connected_ = connected;
    }
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return ""; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return ""; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return ""; }

    // Simulate the upstream link dying underneath the bridge (USB unplug,
    // serial wedge) without going through disconnect().
    void drop_link() {
        connected_ = false;
        cleanup_pending = true;
    }

    bool cleanup_pending = false;

    int connect_count = 0;
    int disconnect_count = 0;

private:
    int number_;
    bool connected_ = false;
};

// Mirrors the Celestron / OnStep / Bisque / iOptron / Sky-Watcher telescopes:
// an AsyncConnectable driver whose get_connected() takes the state mutex that
// set_connected() holds for the whole (slow) connect. The router must never
// read it while a connection task is in flight, or its own connect-wait
// deadline cannot fire and a polling GET connected stalls for the entire
// handshake (issue #130). SynScan is deliberately absent: it was the driver
// that produced #130, and its fix made its getter a bare atomic load, so it
// no longer has this shape -- see async_connectable.h for the full list.
class LockedSlowConnectStubDriver final : public alpacacore::AlpacaDriver, protected alpacacore::AsyncConnectable {
public:
    LockedSlowConnectStubDriver(int number, std::chrono::milliseconds connect_delay)
        : AsyncConnectable("LockedSlowStub"), number_(number), connect_delay_(connect_delay) {}
    ~LockedSlowConnectStubDriver() override { shutdown_connection(); }

    int get_device_number() const override { return number_; }
    std::string get_name() const override { return "Locked Slow Connect Stub"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::CoverCalibrator; }
    std::string get_unique_id() const override { return "locked-slow-stub-" + std::to_string(number_); }
    std::string get_description() const override { return "fake device"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 1; }
    bool get_connected() const override {
        std::lock_guard<std::mutex> lock(mutex_);  // blocks for the whole connect, like the real drivers
        return connected_;
    }
    bool get_connecting() const override { return connection_task_active(); }
    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected) {
            if (record_disconnect_if_connect_in_flight(connected_)) {
                return;
            }
            connected_ = false;
            return;
        }
        if (consume_pending_disconnect(connected_)) {
            return;
        }
        if (connected_) {
            return;
        }
        std::this_thread::sleep_for(connect_delay_);  // the "handshake", mutex held throughout
        connected_ = true;
    }
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return ""; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return ""; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return ""; }

private:
    int number_;
    std::chrono::milliseconds connect_delay_;
    mutable std::mutex mutex_;
    bool connected_ = false;
};

// Issue #358: a driver that refuses a connect and explains why, wired through
// AsyncConnectable the way all 38 real drivers are, so the router path under
// test is the real one.
class RefusingConnectStubDriver final : public alpacacore::AlpacaDriver, protected alpacacore::AsyncConnectable {
public:
    // `protected`, exactly as all 38 shipped drivers mix this base in, and the
    // reason reaches the router through the AlpacaDriver virtual rather than a
    // cross-cast. An earlier version of this stub inherited publicly, which is
    // the ONE shape in the tree that makes a dynamic_cast to AsyncConnectable
    // succeed -- so the test passed while every real driver fell back to the
    // bare constant. Keep it protected: that is what makes this case able to
    // fail.
    ALPACA_EXPOSE_CONNECT_ERROR()

    // Deliberately the multi-clause shape a real driver's guard produces: the
    // sentence that tells the operator what to fix is the whole point, so a
    // test that pinned a one-word message would not show it survives.
    static constexpr const char* kReason =
        "Site latitude and longitude must be set before connecting: this mount stores no site of its own, "
        "and pointing math is hemisphere-dependent";

    explicit RefusingConnectStubDriver(int number) : AsyncConnectable("RefusingStub"), number_(number) {}
    ~RefusingConnectStubDriver() override { shutdown_connection(); }

    int get_device_number() const override { return number_; }
    std::string get_name() const override { return "Refusing Connect Stub"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::CoverCalibrator; }
    std::string get_unique_id() const override { return "refusing-connect-stub-" + std::to_string(number_); }
    std::string get_description() const override { return "fake device"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 1; }
    bool get_connected() const override { return false; }
    bool get_connecting() const override { return connection_task_active(); }
    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    void set_connected(bool connected) override {
        if (connected) {
            throw std::runtime_error(kReason);
        }
    }
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return ""; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return ""; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return ""; }

private:
    int number_;
};

// GET .../connected for a given ClientID (no ClientID when client_id is empty)
// and return the reported Value.
bool get_connected_value(alpacahttp::Router& router, const std::string& path_base, const std::string& client_id,
                         const std::string& remote_addr = std::string()) {
    std::string path = path_base + "/connected";
    if (!client_id.empty()) {
        path += "?ClientID=" + client_id;
    }
    const auto resp = route_request(router, "GET", path, "", remote_addr);
    const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
    EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
    return json.value("Value", false);
}

// PUT .../connected with a form body; expects success unless expect_error.
void put_connected(alpacahttp::Router& router, const std::string& path_base, const std::string& client_id,
                   bool connected, const std::string& remote_addr = std::string()) {
    std::string body = "Connected=" + std::string(connected ? "true" : "false");
    if (!client_id.empty()) {
        body += "&ClientID=" + client_id;
    }
    const auto resp = route_request(router, "PUT", path_base + "/connected", body, remote_addr);
    const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
    EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
}

}  // namespace

int main() {
    // open-astro#354: host_time_zone() resolution table, through the seam
    // (explicit TZ and an /etc stand-in under a temp dir) so the answer does
    // not depend on the build host's own zone.
    {
        using alpacahttp::util::host_time_zone;
        using alpacahttp::util::looks_like_iana_zone;
        namespace fs = std::filesystem;

        EXPECT(looks_like_iana_zone("America/Denver"));
        EXPECT(looks_like_iana_zone("Etc/UTC"));
        EXPECT(looks_like_iana_zone("America/Argentina/Buenos_Aires"));
        EXPECT(looks_like_iana_zone("Etc/GMT+12"));
        EXPECT(!looks_like_iana_zone(""));
        EXPECT(!looks_like_iana_zone("UTC"));                     // no '/': a slash-free tzdb name, reported as unknown
        EXPECT(!looks_like_iana_zone("EST5EDT"));                 // likewise; the same shape as a POSIX rule prefix
        EXPECT(!looks_like_iana_zone("EST5EDT,M3.2.0,M11.1.0"));  // POSIX rule string, not an IANA name
        EXPECT(!looks_like_iana_zone("localtime"));               // zoneinfo/ files that are not zones: no '/'
        EXPECT(!looks_like_iana_zone("posixrules"));
        EXPECT(!looks_like_iana_zone("/America/Denver"));
        EXPECT(!looks_like_iana_zone("America/Denver/"));
        EXPECT(!looks_like_iana_zone("America//Denver"));
        EXPECT(!looks_like_iana_zone("America/Den ver"));
        EXPECT(!looks_like_iana_zone("../../etc/passwd"));

        char tmpl[] = "/tmp/ab_tz_etc_XXXXXX";
        const char* etc = ::mkdtemp(tmpl);
        EXPECT(etc != nullptr);
        const std::string etc_dir = etc ? etc : "";
        const std::string zoneinfo = etc_dir + "/zoneinfo";
        fs::create_directories(zoneinfo + "/Pacific");
        { std::ofstream(zoneinfo + "/Pacific/Auckland") << "TZif"; }

        // Nothing configured: "".
        EXPECT(host_time_zone(nullptr, etc_dir) == "");

        // TZ wins outright, with the tzset() ':' prefix stripped and the
        // absolute-path form reduced to its zone.
        EXPECT(host_time_zone("Pacific/Auckland", etc_dir) == "Pacific/Auckland");
        EXPECT(host_time_zone(":Pacific/Auckland", etc_dir) == "Pacific/Auckland");
        EXPECT(host_time_zone(":/usr/share/zoneinfo/Pacific/Auckland", etc_dir) == "Pacific/Auckland");
        // A zoneinfo-relative TZ in the posix/ or right/ subtree, absolute or not.
        EXPECT(host_time_zone("posix/Pacific/Auckland", etc_dir) == "Pacific/Auckland");
        EXPECT(host_time_zone(":/usr/share/zoneinfo/right/Pacific/Auckland", etc_dir) == "Pacific/Auckland");

        // /etc/timezone, trimmed.
        { std::ofstream(etc_dir + "/timezone") << "America/Denver\n"; }
        EXPECT(host_time_zone(nullptr, etc_dir) == "America/Denver");
        // A set-but-unusable TZ governs localtime_r() and must not be
        // contradicted by the file: "" rather than "America/Denver".
        EXPECT(host_time_zone("EST5EDT", etc_dir) == "");
        EXPECT(host_time_zone("", etc_dir) == "");

        // A junk /etc/timezone and no symlink: "".
        { std::ofstream(etc_dir + "/timezone") << "not a zone\n"; }
        EXPECT(host_time_zone(nullptr, etc_dir) == "");
        fs::create_symlink(zoneinfo + "/Pacific/Auckland", etc_dir + "/localtime");
        EXPECT(host_time_zone(nullptr, etc_dir) == "Pacific/Auckland");
        // Debian's relative symlink form resolves the same way.
        fs::remove(etc_dir + "/localtime");
        fs::create_symlink("../usr/share/zoneinfo/Etc/UTC", etc_dir + "/localtime");
        EXPECT(host_time_zone(nullptr, etc_dir) == "Etc/UTC");
        // The symlink outranks /etc/timezone when the two disagree: glibc's
        // tzset() reads /etc/localtime and never /etc/timezone, so the link
        // is the zone localtime_r() (and the log lines) actually use.
        { std::ofstream(etc_dir + "/timezone") << "Europe/London\n"; }
        EXPECT(host_time_zone(nullptr, etc_dir) == "Etc/UTC");
        // The symlink answering with a name this resolver cannot express
        // (timedatectl set-timezone UTC -> zoneinfo/UTC, no '/') is NOT the
        // symlink being absent: the file must not get to contradict it.
        fs::remove(etc_dir + "/localtime");
        fs::create_symlink("../usr/share/zoneinfo/UTC", etc_dir + "/localtime");
        EXPECT(host_time_zone(nullptr, etc_dir) == "");
        fs::remove(etc_dir + "/localtime");
        fs::create_symlink("../usr/share/zoneinfo/Etc/UTC", etc_dir + "/localtime");
        // A "posix/" or "right/" zoneinfo subtree is the same zone under a
        // name Intl rejects; the prefix is stripped.
        fs::remove(etc_dir + "/localtime");
        fs::create_symlink("/usr/share/zoneinfo/right/Europe/Berlin", etc_dir + "/localtime");
        EXPECT(host_time_zone(nullptr, etc_dir) == "Europe/Berlin");
        fs::remove(etc_dir + "/localtime");
        fs::create_symlink("/usr/share/zoneinfo/posix/Europe/Berlin", etc_dir + "/localtime");
        EXPECT(host_time_zone(nullptr, etc_dir) == "Europe/Berlin");
        // A regular-file /etc/localtime (no symlink) falls back to the file.
        fs::remove(etc_dir + "/localtime");
        { std::ofstream(etc_dir + "/localtime") << "TZif"; }
        EXPECT(host_time_zone(nullptr, etc_dir) == "Europe/London");
        { std::ofstream(etc_dir + "/timezone") << "not a zone\n"; }
        EXPECT(host_time_zone(nullptr, etc_dir) == "");

        fs::remove_all(etc_dir);
    }
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

    // Conformance: "Parameter names are not case sensitive, so clients and
    // drivers should be prepared for parameter names to be supplied ... with
    // any casing." Query parameter lookups must match regardless of casing.
    EXPECT(request.has_query_param("rightascension"));
    EXPECT(request.has_query_param("DECLINATION"));
    EXPECT(request.get_query_param("RIGHTASCENSION") == "1.5");
    EXPECT(request.get_query_param("declination") == "-20.3");
    EXPECT(!request.has_query_param("nonexistent"));

    // Conformance: HTTP 400 "indicates that the device could not interpret the
    // request e.g. an invalid device number or misspelt device type". These
    // must be 400 (Bad Request), not 404, and carry a non-zero ErrorNumber.
    {
        // Misspelt / unknown device type.
        const auto resp = route_request(router, "GET", "/api/v1/wibble/0/connected");
        EXPECT(resp.status_code() == 400);
        const auto json = nlohmann::json::parse(resp.body());
        EXPECT(json.value("ErrorNumber", 0) != 0);
    }
    {
        // Valid device type, unknown method.
        const auto resp = route_request(router, "GET", "/api/v1/telescope/0/notarealmethod");
        EXPECT(resp.status_code() == 400);
        const auto json = nlohmann::json::parse(resp.body());
        EXPECT(json.value("ErrorNumber", 0) != 0);
    }
    {
        // Valid type and method, but no device registered at that number.
        const auto resp = route_request(router, "GET", "/api/v1/telescope/4242/connected");
        EXPECT(resp.status_code() == 400);
        const auto json = nlohmann::json::parse(resp.body());
        EXPECT(json.value("ErrorNumber", 0) != 0);
    }

    {
        // #515: CanMoveAxis and AxisRates default to axis 0 when the Axis
        // query parameter is absent, instead of raising InvalidValue like
        // every other required-parameter telescope method. Vendor-free stub
        // so this doesn't need any ALPACACORE_ENABLE_* vendor.
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto scope = std::make_shared<TelescopeClockStubDriver>(9660);
        EXPECT(registry.register_device(scope));
        const std::string base = "/api/v1/telescope/9660";

        {
            const auto resp = route_request(router, "GET", base + "/canmoveaxis");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        }
        {
            const auto resp = route_request(router, "GET", base + "/axisrates");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        }
        // With Axis explicitly supplied, both succeed.
        {
            const auto resp = route_request(router, "GET", base + "/canmoveaxis?Axis=0");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
        }
        {
            const auto resp = route_request(router, "GET", base + "/axisrates?Axis=0");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
        }

        registry.unregister_device(alpacacore::DeviceType::Telescope, 9660);
    }

    {
        // open-astro#547: the router's single device-dispatch choke point
        // stamps client activity on the telescope for ANY request addressed
        // to it (including the client's own Slewing polls), and only for
        // that device -- not for a request to a different device number.
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto stub = std::make_shared<TelescopeClockStubDriver>(9850);
        auto other = std::make_shared<TelescopeClockStubDriver>(9851);
        EXPECT(registry.register_device(stub));
        EXPECT(registry.register_device(other));

        EXPECT(!stub->last_client_activity().has_value());

        route_request(router, "GET", "/api/v1/telescope/9850/connected");
        EXPECT(stub->last_client_activity().has_value());
        const auto first_stamp = *stub->last_client_activity();

        // A different device's traffic must not stamp this one.
        route_request(router, "GET", "/api/v1/telescope/9851/connected");
        EXPECT(*stub->last_client_activity() == first_stamp);

        // The client's own Slewing poll counts too (no per-endpoint list).
        // Strict '>' (not '>=') -- steady_clock is monotonic, so '>=' would
        // pass even if this request never re-stamped anything.
        route_request(router, "GET", "/api/v1/telescope/9850/slewing");
        EXPECT(*stub->last_client_activity() > first_stamp);

        registry.unregister_device(alpacacore::DeviceType::Telescope, 9850);
        registry.unregister_device(alpacacore::DeviceType::Telescope, 9851);
    }

    {
        // #574: three router-level request-validation gaps, all vendor-free
        // against a fresh TelescopeClockStubDriver (its set_target_declination
        // is a no-op, so a NaN that reached the driver would return
        // ErrorNumber 0 rather than throw — making the rejection here
        // unambiguously the router's, not the driver's).
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto scope = std::make_shared<TelescopeClockStubDriver>(9574);
        EXPECT(registry.register_device(scope));
        const std::string base = "/api/v1/telescope/9574";

        // 1. Non-finite / hex-float doubles are rejected with InvalidValue
        // before reaching the driver's setter.
        for (const std::string& raw : {"nan", "inf", "-infinity", "0x1p3"}) {
            const auto resp =
                route_request(router, "PUT", base + "/targetdeclination", "TargetDeclination=" + raw + "&ClientID=1");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(resp.status_code() == 200);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) == 0x401);
        }

        // 2. A valid method on the wrong verb is rejected with 400 before the
        // ASCOM operation, not translated into a 200/0x400 NotImplemented.
        {
            // "altitude" is GET-only.
            const auto resp = route_request(router, "PUT", base + "/altitude", "ClientID=1");
            EXPECT(resp.status_code() == 400);
        }
        {
            // "abortslew" is PUT-only.
            const auto resp = route_request(router, "GET", base + "/abortslew");
            EXPECT(resp.status_code() == 400);
        }
        {
            // "action" (a common method) is PUT-only.
            const auto resp = route_request(router, "GET", base + "/action");
            EXPECT(resp.status_code() == 400);
        }
        {
            // Positive control: GET altitude (the correct verb) still works.
            const auto resp = route_request(router, "GET", base + "/altitude");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(resp.status_code() == 200);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
        }
        // A verb no Alpaca endpoint accepts (POST, DELETE) is a wrong verb
        // too; a foreign Origin is still refused with 403 first, so the
        // cross-origin guard UTCDate carries (#401) stays in front.
        {
            const auto send = [&](const std::string& verb, const std::string& member, const std::string& origin) {
                std::ostringstream raw;
                raw << verb << " " << base << "/" << member << " HTTP/1.1\r\n"
                    << "Host: localhost\r\n";
                if (!origin.empty()) {
                    raw << "Origin: " << origin << "\r\n";
                }
                raw << "\r\n";
                alpacahttp::Request request;
                EXPECT(request.parse(raw.str()));
                return router.route(request, 1);
            };
            EXPECT(send("POST", "altitude", "").status_code() == 400);
            EXPECT(send("DELETE", "abortslew", "").status_code() == 400);
            EXPECT(send("POST", "utcdate", "").status_code() == 400);
            EXPECT(send("POST", "utcdate", "http://evil.example").status_code() == 403);
            EXPECT(send("DELETE", "altitude", "http://evil.example").status_code() == 403);
            // The Site* setters' #444 guard sits in their PUT branch only, so
            // a forged POST reaches the router's verb check; it must be
            // refused with 403 there, not answered 400 (or 200/0x400 before #574).
            EXPECT(send("POST", "sitelatitude", "http://evil.example").status_code() == 403);
            EXPECT(send("POST", "sitelatitude", "").status_code() == 400);
        }

        // 3. Device numbers that don't fit in a uint32_t are rejected with
        // 400 instead of wrapping (2^32 -> N) or surfacing as an internal
        // DRIVER_ERROR 200 (a digit string overflowing even a 64-bit parse).
        //
        // The wrap case needs a device registered at the number the old cast
        // wraps to: 4294976870 is 2^32 + 9574, which wrapped to 9574, and
        // 9574 is still registered here (it is unregistered just below), so
        // the pre-fix router answered 200. The obvious 4294967296 would be
        // vacuous: it wraps to device 0, which is not registered, and the
        // old router already answered 400 "Device not found" for it.
        {
            const auto resp = route_request(router, "GET", "/api/v1/telescope/4294976870/connected");
            EXPECT(resp.status_code() == 400);
        }

        registry.unregister_device(alpacacore::DeviceType::Telescope, 9574);

        {
            const auto resp = route_request(router, "GET", "/api/v1/telescope/99999999999999999999/connected");
            EXPECT(resp.status_code() == 400);
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        }
    }

#ifdef ALPACACORE_ENABLE_ZWO
    // Ensure idempotent behavior across repeated test runs.
    {
        nlohmann::json remove_body = {{"vendor", "zwo"}, {"deviceType", "telescope"}, {"deviceNumber", 9101}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        nlohmann::json configure_body = {{"vendor", "zwo"},           {"deviceType", "telescope"},
                                         {"deviceNumber", 9101},      {"connectionType", "serial"},
                                         {"portPath", "/dev/null"},   {"baudRate", 9600},
                                         {"responseTimeoutMs", 2500}, {"apertureDiameter", 0.1},
                                         {"focalLength", 0.8},        {"siteLatitude", 34.5},
                                         {"siteLongitude", -117.2},   {"siteElevation", 450.0},
                                         {"syncTimeOnConnect", false}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
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
            if (entry.value("DeviceType", "") == "Telescope" && entry.value("DeviceNumber", -1) == 9101) {
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

        nlohmann::json remove_body = {{"vendor", "zwo"}, {"deviceType", "telescope"}, {"deviceNumber", 9101}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
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

    // --- ToupTek thermal switch routing test ---
    // The (touptek, switch) route selects between the StellaVita PowerBox and
    // the cooled-camera thermal switch (dew heater / fan / tail LED) via
    // switchType. The "thermal" backend is available on any ToupTek build (it
    // needs no libgpiod), registers without hardware, and must round-trip its
    // switchType discriminator and cameraIndex binding.
#ifdef ALPACACORE_ENABLE_TOUPTEK
    {
        nlohmann::json configure_body = {{"vendor", "touptek"},
                                         {"deviceType", "switch"},
                                         {"deviceNumber", 9181},
                                         {"switchType", "thermal"},
                                         {"cameraIndex", 2}};
        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        bool found_touptek_thermal = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Switch" && entry.value("DeviceNumber", -1) == 9181) {
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "touptek");
                EXPECT(cfg.value("switchType", "") == "thermal");
                EXPECT(cfg.value("cameraIndex", -1) == 2);
                found_touptek_thermal = true;
                break;
            }
        }
        EXPECT(found_touptek_thermal);

        nlohmann::json remove_body = {{"vendor", "touptek"}, {"deviceType", "switch"}, {"deviceNumber", 9181}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        EXPECT(nlohmann::json::parse(remove_response.body()).value("ErrorNumber", -1) == 0);
    }
    {
        // An unknown/typo'd switchType (e.g. wrong case "Thermal") must be
        // rejected, never silently fall through to a StellaVita registration.
        nlohmann::json configure_body = {
            {"vendor", "touptek"}, {"deviceType", "switch"}, {"deviceNumber", 9182}, {"switchType", "Thermal"}};
        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
    }
#endif

    // --- Player One thermal switch routing test ---
#ifdef ALPACACORE_ENABLE_PLAYERONE
    {
        // Runtime heater/fan control is switch-only by design: no connect-time
        // heater/fan camera config exists (a persisted "heater on" would
        // silently re-apply months later), so there is nothing camera-side to
        // round-trip beyond cameraIndex.
        nlohmann::json configure_body = {
            {"vendor", "playerone"}, {"deviceType", "camera"}, {"deviceNumber", 9210}, {"cameraIndex", 0}};
        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        nlohmann::json remove_body = {{"vendor", "playerone"}, {"deviceType", "camera"}, {"deviceNumber", 9210}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        EXPECT(nlohmann::json::parse(remove_response.body()).value("ErrorNumber", -1) == 0);
    }
    {
        // Thermal switch (dew heater / fan) registers without hardware and
        // persists its camera index.
        nlohmann::json configure_body = {
            {"vendor", "playerone"}, {"deviceType", "switch"}, {"deviceNumber", 9211}, {"cameraIndex", 1}};
        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        bool found_playerone_switch = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Switch" && entry.value("DeviceNumber", -1) == 9211) {
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "playerone");
                EXPECT(cfg.value("cameraIndex", -1) == 1);
                found_playerone_switch = true;
                break;
            }
        }
        EXPECT(found_playerone_switch);

        nlohmann::json remove_body = {{"vendor", "playerone"}, {"deviceType", "switch"}, {"deviceNumber", 9211}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        EXPECT(nlohmann::json::parse(remove_response.body()).value("ErrorNumber", -1) == 0);
    }
#endif

    // --- Celestron telescope routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_CELESTRON
    {
        nlohmann::json remove_body = {{"vendor", "celestron"}, {"deviceType", "telescope"}, {"deviceNumber", 9102}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        nlohmann::json configure_body = {
            {"vendor", "celestron"},      {"deviceType", "telescope"}, {"deviceNumber", 9102},
            {"connectionType", "serial"}, {"portPath", "/dev/null"},   {"baudRate", 9600},
            {"responseTimeoutMs", 5000},  {"apertureDiameter", 0.28},  {"focalLength", 2.8},
            {"siteLatitude", 33.85},      {"siteLongitude", -118.34},  {"siteElevation", 100.0},
            {"syncTimeOnConnect", true}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
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
            if (entry.value("DeviceType", "") == "Telescope" && entry.value("DeviceNumber", -1) == 9102) {
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
        nlohmann::json net_configure_body = {{"vendor", "celestron"},   {"deviceType", "telescope"},
                                             {"deviceNumber", 9103},    {"connectionType", "network"},
                                             {"host", "192.168.1.100"}, {"tcpPort", 2000}};

        const auto net_response =
            route_request(router, "POST", "/management/v1/configuredevice", net_configure_body.dump());
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
        nlohmann::json remove_body = {{"vendor", "celestron"}, {"deviceType", "telescope"}, {"deviceNumber", 9102}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);

        nlohmann::json remove_net_body = {{"vendor", "celestron"}, {"deviceType", "telescope"}, {"deviceNumber", 9103}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_net_body.dump());
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- ToupTek camera routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_TOUPTEK
    {
        nlohmann::json remove_body = {{"vendor", "touptek"}, {"deviceType", "camera"}, {"deviceNumber", 9201}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        nlohmann::json configure_body = {
            {"vendor", "touptek"}, {"deviceType", "camera"}, {"deviceNumber", 9201}, {"cameraIndex", 2}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
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
            if (entry.value("DeviceType", "") == "Camera" && entry.value("DeviceNumber", -1) == 9201) {
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

        nlohmann::json remove_body = {{"vendor", "touptek"}, {"deviceType", "camera"}, {"deviceNumber", 9201}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- ToupTek AAF focuser routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_TOUPTEK
    {
        nlohmann::json remove_body = {{"vendor", "touptek"}, {"deviceType", "focuser"}, {"deviceNumber", 9202}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        nlohmann::json configure_body = {{"vendor", "touptek"},
                                         {"deviceType", "focuser"},
                                         {"deviceNumber", 9202},
                                         {"focuserIndex", 0},
                                         {"focuserId", "tp-aaf-routing-test"}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
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
            if (entry.value("DeviceType", "") == "Focuser" && entry.value("DeviceNumber", -1) == 9202) {
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

        nlohmann::json remove_body = {{"vendor", "touptek"}, {"deviceType", "focuser"}, {"deviceNumber", 9202}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- ToupTek AFW filter wheel routing/config persistence test ---
    // Guards against sanitize_device_config dropping the filter-wheel binding
    // and custom filter names on save (a strict allowlist silently strips any
    // field it does not copy).
#ifdef ALPACACORE_ENABLE_TOUPTEK
    {
        nlohmann::json remove_body = {{"vendor", "touptek"}, {"deviceType", "filterwheel"}, {"deviceNumber", 9203}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        const std::vector<std::string> filter_names = {"Lum", "Red", "Green", "Blue", "Ha"};
        nlohmann::json configure_body = {{"vendor", "touptek"},
                                         {"deviceType", "filterwheel"},
                                         {"deviceNumber", 9203},
                                         {"filterwheelIndex", 0},
                                         {"filterwheelId", "tp-afw-routing-test"},
                                         {"filterNames", filter_names}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_TOUPTEK
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_touptek_wheel = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "FilterWheel" && entry.value("DeviceNumber", -1) == 9203) {
                EXPECT(entry.value("Vendor", "") == "touptek");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "touptek");
                EXPECT(cfg.value("deviceType", "") == "filterwheel");
                // The three fields that sanitize_device_config used to strip.
                EXPECT(cfg.value("filterwheelId", "") == "tp-afw-routing-test");
                EXPECT(cfg.contains("filterwheelIndex"));
                EXPECT(cfg.contains("filterNames"));
                EXPECT(cfg["filterNames"] == filter_names);
                found_touptek_wheel = true;
                break;
            }
        }
        EXPECT(found_touptek_wheel);

        nlohmann::json remove_body = {{"vendor", "touptek"}, {"deviceType", "filterwheel"}, {"deviceNumber", 9203}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- QHY integrated CFW filter wheel routing/config persistence test ---
    // Guards against sanitize_device_config dropping filterNames on save (the
    // qhy branch only allowlisted cameraIndex/cameraId until this was added).
#ifdef ALPACACORE_ENABLE_QHY
    {
        nlohmann::json remove_body = {{"vendor", "qhy"}, {"deviceType", "filterwheel"}, {"deviceNumber", 9204}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        const std::vector<std::string> filter_names = {"Lum", "Red", "Green", "Blue", "Ha"};
        nlohmann::json configure_body = {{"vendor", "qhy"},
                                         {"deviceType", "filterwheel"},
                                         {"deviceNumber", 9204},
                                         {"cameraIndex", 0},
                                         {"filterNames", filter_names}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_QHY
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_qhy_wheel = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "FilterWheel" && entry.value("DeviceNumber", -1) == 9204) {
                EXPECT(entry.value("Vendor", "") == "qhy");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "qhy");
                EXPECT(cfg.value("deviceType", "") == "filterwheel");
                EXPECT(cfg.contains("cameraIndex"));
                EXPECT(cfg.contains("filterNames"));
                EXPECT(cfg["filterNames"] == filter_names);
                found_qhy_wheel = true;
                break;
            }
        }
        EXPECT(found_qhy_wheel);

        nlohmann::json remove_body = {{"vendor", "qhy"}, {"deviceType", "filterwheel"}, {"deviceNumber", 9204}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
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

    // --- WandererAstro CoverCalibrator routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
    {
        nlohmann::json remove_body = {
            {"vendor", "wandererastro"}, {"deviceType", "covercalibrator"}, {"deviceNumber", 9401}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        // Serial mode with an explicit (dummy) port avoids the auto-detect scan
        // and registers the driver without opening a real device.
        nlohmann::json configure_body = {{"vendor", "wandererastro"}, {"deviceType", "covercalibrator"},
                                         {"deviceNumber", 9401},      {"connectionType", "serial"},
                                         {"portPath", "/dev/null"},   {"baudRate", 19200}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_cover = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "CoverCalibrator" && entry.value("DeviceNumber", -1) == 9401) {
                EXPECT(entry.value("Vendor", "") == "wandererastro");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "wandererastro");
                EXPECT(cfg.value("deviceType", "") == "covercalibrator");
                EXPECT(cfg.value("connectionType", "") == "serial");
                EXPECT(cfg.value("portPath", "") == "/dev/null");
                EXPECT(cfg.value("baudRate", -1) == 19200);
                found_cover = true;
                break;
            }
        }
        EXPECT(found_cover);

        // MaxBrightness is a static capability and reports without hardware.
        const auto maxbright_response = route_request(router, "GET", "/api/v1/covercalibrator/9401/maxbrightness");
        const auto maxbright_json = nlohmann::json::parse(maxbright_response.body());
        EXPECT(maxbright_json.value("ErrorNumber", -1) == 0);
        EXPECT(maxbright_json.value("Value", -1) == 255);

        nlohmann::json remove_body = {
            {"vendor", "wandererastro"}, {"deviceType", "covercalibrator"}, {"deviceNumber", 9401}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- WandererAstro Rotator routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
    {
        nlohmann::json remove_body = {{"vendor", "wandererastro"}, {"deviceType", "rotator"}, {"deviceNumber", 9402}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        // Serial mode with an explicit (dummy) port avoids the auto-detect scan
        // and registers the driver without opening a real device.
        nlohmann::json configure_body = {{"vendor", "wandererastro"}, {"deviceType", "rotator"},
                                         {"deviceNumber", 9402},      {"connectionType", "serial"},
                                         {"portPath", "/dev/null"},   {"baudRate", 19200}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);

        bool found_rotator = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Rotator" && entry.value("DeviceNumber", -1) == 9402) {
                EXPECT(entry.value("Vendor", "") == "wandererastro");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "wandererastro");
                EXPECT(cfg.value("deviceType", "") == "rotator");
                EXPECT(cfg.value("connectionType", "") == "serial");
                EXPECT(cfg.value("portPath", "") == "/dev/null");
                EXPECT(cfg.value("baudRate", -1) == 19200);
                found_rotator = true;
                break;
            }
        }
        EXPECT(found_rotator);

        // CanReverse and StepSize are static capabilities that report without
        // hardware (1142 steps/degree worm drive).
        const auto canreverse_response = route_request(router, "GET", "/api/v1/rotator/9402/canreverse");
        const auto canreverse_json = nlohmann::json::parse(canreverse_response.body());
        EXPECT(canreverse_json.value("ErrorNumber", -1) == 0);
        EXPECT(canreverse_json.value("Value", false) == true);

        nlohmann::json remove_body = {{"vendor", "wandererastro"}, {"deviceType", "rotator"}, {"deviceNumber", 9402}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- WandererAstro FilterWheel routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
    {
        nlohmann::json remove_body = {
            {"vendor", "wandererastro"}, {"deviceType", "filterwheel"}, {"deviceNumber", 9403}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        // Serial mode with an explicit (dummy) port avoids the auto-detect scan
        // and registers the driver without opening a real device.
        nlohmann::json configure_body = {{"vendor", "wandererastro"},
                                         {"deviceType", "filterwheel"},
                                         {"deviceNumber", 9403},
                                         {"connectionType", "serial"},
                                         {"portPath", "/dev/null"},
                                         {"baudRate", 19200},
                                         {"filterNames", {"L", "R", "G", "B", "Ha", "OIII", "SII", "Clear"}}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);

        bool found_filterwheel = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "FilterWheel" && entry.value("DeviceNumber", -1) == 9403) {
                EXPECT(entry.value("Vendor", "") == "wandererastro");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "wandererastro");
                EXPECT(cfg.value("deviceType", "") == "filterwheel");
                EXPECT(cfg.value("connectionType", "") == "serial");
                EXPECT(cfg.value("portPath", "") == "/dev/null");
                EXPECT(cfg.value("baudRate", -1) == 19200);
                EXPECT(cfg.contains("filterNames"));
                EXPECT(cfg["filterNames"].size() == 8);
                EXPECT(cfg["filterNames"][4] == "Ha");
                found_filterwheel = true;
                break;
            }
        }
        EXPECT(found_filterwheel);

        // Names and FocusOffsets are driver-side state that report without
        // hardware (the whole Wanderer lineup is fixed at 8 slots).
        const auto names_response = route_request(router, "GET", "/api/v1/filterwheel/9403/names");
        const auto names_json = nlohmann::json::parse(names_response.body());
        EXPECT(names_json.value("ErrorNumber", -1) == 0);
        EXPECT(names_json["Value"].size() == 8);
        EXPECT(names_json["Value"][0] == "L");

        nlohmann::json remove_body = {
            {"vendor", "wandererastro"}, {"deviceType", "filterwheel"}, {"deviceNumber", 9403}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- WandererAstro WandererBox Pro V3 Switch routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
    {
        nlohmann::json remove_body = {{"vendor", "wandererastro"}, {"deviceType", "switch"}, {"deviceNumber", 9404}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        // Serial mode with an explicit (dummy) port avoids the auto-detect scan
        // and registers the driver without opening a real device.
        nlohmann::json configure_body = {{"vendor", "wandererastro"},  {"deviceType", "switch"},
                                         {"deviceNumber", 9404},       {"switchType", "wandererbox-pro-v3"},
                                         {"connectionType", "serial"}, {"portPath", "/dev/null"},
                                         {"baudRate", 19200}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);

        bool found_switch = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Switch" && entry.value("DeviceNumber", -1) == 9404) {
                EXPECT(entry.value("Vendor", "") == "wandererastro");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "wandererastro");
                EXPECT(cfg.value("deviceType", "") == "switch");
                EXPECT(cfg.value("switchType", "") == "wandererbox-pro-v3");
                EXPECT(cfg.value("connectionType", "") == "serial");
                EXPECT(cfg.value("portPath", "") == "/dev/null");
                EXPECT(cfg.value("baudRate", -1) == 19200);
                found_switch = true;
                break;
            }
        }
        EXPECT(found_switch);

        // MaxSwitch is a static capability that reports without hardware (the
        // Pro V3 exposes 14 outputs + 10 sensor values).
        const auto maxswitch_response = route_request(router, "GET", "/api/v1/switch/9404/maxswitch");
        const auto maxswitch_json = nlohmann::json::parse(maxswitch_response.body());
        EXPECT(maxswitch_json.value("ErrorNumber", -1) == 0);
        EXPECT(maxswitch_json.value("Value", -1) == 24);

        // An unknown switchType must be rejected with a clear error.
        nlohmann::json bad_body = {{"vendor", "wandererastro"},
                                   {"deviceType", "switch"},
                                   {"deviceNumber", 9405},
                                   {"switchType", "not-a-backend"}};
        const auto bad_response = route_request(router, "POST", "/management/v1/configuredevice", bad_body.dump());
        const auto bad_json = nlohmann::json::parse(bad_response.body());
        EXPECT(bad_json.value("ErrorNumber", 0) != 0);

        nlohmann::json remove_body = {{"vendor", "wandererastro"}, {"deviceType", "switch"}, {"deviceNumber", 9404}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // --- Gemini Power & Data Hubs Advanced 3 Switch routing/config persistence test ---
#ifdef ALPACACORE_ENABLE_GEMINI
    {
        nlohmann::json remove_body = {{"vendor", "gemini"}, {"deviceType", "switch"}, {"deviceNumber", 9408}};
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
    }
#endif

    {
        // Serial mode with an explicit (dummy) port avoids the auto-detect scan
        // and registers the driver without opening a real device.
        nlohmann::json configure_body = {
            {"vendor", "gemini"},         {"deviceType", "switch"},  {"deviceNumber", 9408}, {"switchType", "pdh-adv3"},
            {"connectionType", "serial"}, {"portPath", "/dev/null"}, {"baudRate", 19200}};

        const auto configure_response =
            route_request(router, "POST", "/management/v1/configuredevice", configure_body.dump());
        const auto configure_json = nlohmann::json::parse(configure_response.body());

#ifdef ALPACACORE_ENABLE_GEMINI
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);

        bool found_switch = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Switch" && entry.value("DeviceNumber", -1) == 9408) {
                EXPECT(entry.value("Vendor", "") == "gemini");
                EXPECT(entry.contains("Config"));
                const auto& cfg = entry["Config"];
                EXPECT(cfg.value("vendor", "") == "gemini");
                EXPECT(cfg.value("deviceType", "") == "switch");
                EXPECT(cfg.value("switchType", "") == "pdh-adv3");
                EXPECT(cfg.value("connectionType", "") == "serial");
                EXPECT(cfg.value("portPath", "") == "/dev/null");
                EXPECT(cfg.value("baudRate", -1) == 19200);
                found_switch = true;
                break;
            }
        }
        EXPECT(found_switch);

        // MaxSwitch is a static capability that reports without hardware (the
        // Advanced 3 exposes 15 outputs/modes + 9 telemetry values).
        const auto maxswitch_response = route_request(router, "GET", "/api/v1/switch/9408/maxswitch");
        const auto maxswitch_json = nlohmann::json::parse(maxswitch_response.body());
        EXPECT(maxswitch_json.value("ErrorNumber", -1) == 0);
        EXPECT(maxswitch_json.value("Value", -1) == 24);

        // An unknown switchType must be rejected with a clear error.
        nlohmann::json bad_body = {
            {"vendor", "gemini"}, {"deviceType", "switch"}, {"deviceNumber", 9409}, {"switchType", "not-a-backend"}};
        const auto bad_response = route_request(router, "POST", "/management/v1/configuredevice", bad_body.dump());
        const auto bad_json = nlohmann::json::parse(bad_response.body());
        EXPECT(bad_json.value("ErrorNumber", 0) != 0);

        nlohmann::json remove_body = {{"vendor", "gemini"}, {"deviceType", "switch"}, {"deviceNumber", 9408}};
        const auto remove_response = route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        const auto remove_json = nlohmann::json::parse(remove_response.body());
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
#endif
    }

    // =====================================================================
    // Issue #102 back-fill: config save->load round-trips for every
    // (vendor, deviceType) that persists fields. Each block POSTs distinctive
    // values, reads configureddevices back, and asserts EVERY persisted field
    // survived sanitize_device_config. Required Test Case #6 for each driver.
    // Device numbers 96xx.
    // =====================================================================

#ifdef ALPACACORE_ENABLE_ZWO
    {
        // zwo / camera
        const auto cfg = roundtrip_config(
            router,
            {{"vendor", "zwo"}, {"deviceType", "camera"}, {"deviceNumber", 9601}, {"cameraIndex", 1}, {"cameraId", 7}},
            "Camera", 9601);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("cameraIndex", -1) == 1);
        EXPECT(cfg.value("cameraId", -1) == 7);
        remove_device(router, "zwo", "camera", 9601);
    }
    {
        // zwo / filterwheel
        const auto cfg =
            roundtrip_config(router,
                             {{"vendor", "zwo"},
                              {"deviceType", "filterwheel"},
                              {"deviceNumber", 9602},
                              {"filterwheelIndex", 1},
                              {"filterwheelId", 5},
                              {"filterNames", nlohmann::json::array({"Lum", "Red", "Green", "Blue", "Ha"})}},
                             "FilterWheel", 9602);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("filterwheelIndex", -1) == 1);
        EXPECT(cfg.value("filterwheelId", -1) == 5);
        EXPECT(cfg.contains("filterNames"));
        EXPECT(cfg["filterNames"].size() == 5);
        EXPECT(cfg["filterNames"][4] == "Ha");
        remove_device(router, "zwo", "filterwheel", 9602);
    }
    {
        // zwo / focuser
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "zwo"},
                                           {"deviceType", "focuser"},
                                           {"deviceNumber", 9603},
                                           {"focuserIndex", 1},
                                           {"focuserId", 3}},
                                          "Focuser", 9603);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("focuserIndex", -1) == 1);
        EXPECT(cfg.value("focuserId", -1) == 3);
        remove_device(router, "zwo", "focuser", 9603);
    }
    {
        // zwo / rotator
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "zwo"},
                                           {"deviceType", "rotator"},
                                           {"deviceNumber", 9604},
                                           {"rotatorIndex", 1},
                                           {"rotatorId", 2}},
                                          "Rotator", 9604);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("rotatorIndex", -1) == 1);
        EXPECT(cfg.value("rotatorId", -1) == 2);
        remove_device(router, "zwo", "rotator", 9604);
    }
    {
        // zwo / switch (dew heater — the default switchType)
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "zwo"},
                                           {"deviceType", "switch"},
                                           {"deviceNumber", 9605},
                                           {"switchType", "dewheater"},
                                           {"cameraIndex", 1},
                                           {"cameraId", 4}},
                                          "Switch", 9605);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("switchType", "") == "dewheater");
        EXPECT(cfg.value("cameraIndex", -1) == 1);
        EXPECT(cfg.value("cameraId", -1) == 4);
        remove_device(router, "zwo", "switch", 9605);
    }
    {
        // zwo / switch (ASIAIR Pro/CM4 — libgpiod backend): gpioChip +
        // pwmFrequencyHz + per-port gpio/name/pwm must all survive (the ports
        // array is copied wholesale; a deep-filter regression would strip gpio).
        nlohmann::json ports = nlohmann::json::array(
            {{{"gpio", 12}, {"name", "Mount"}, {"pwm", false}}, {{"gpio", 13}, {"name", "Dew Heater"}, {"pwm", true}}});
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "zwo"},
                                           {"deviceType", "switch"},
                                           {"deviceNumber", 9606},
                                           {"switchType", "asiair"},
                                           {"gpioChip", "/dev/gpiochip0"},
                                           {"pwmFrequencyHz", 200},
                                           {"ports", ports}},
                                          "Switch", 9606);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("switchType", "") == "asiair");
        EXPECT(cfg.value("gpioChip", "") == "/dev/gpiochip0");
        EXPECT(cfg.value("pwmFrequencyHz", -1) == 200);
        EXPECT(cfg.contains("ports"));
        EXPECT(cfg["ports"].size() == 2);
        EXPECT(cfg["ports"][0].value("gpio", -1) == 12);
        EXPECT(cfg["ports"][1].value("name", "") == "Dew Heater");
        EXPECT(cfg["ports"][1].value("pwm", false) == true);
        remove_device(router, "zwo", "switch", 9606);
    }
    {
        // zwo / switch (ASIAIR Plus RK3568 — kernel-module backend):
        // devicePath instead of gpioChip; ports entries carry name/pwm only.
        nlohmann::json ports = nlohmann::json::array({{{"name", "DC1"}, {"pwm", true}}});
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "zwo"},
                                           {"deviceType", "switch"},
                                           {"deviceNumber", 9607},
                                           {"switchType", "asiair-plus-rk3568"},
                                           {"devicePath", "/dev/pwm-gpio-misc"},
                                           {"pwmFrequencyHz", 50},
                                           {"ports", ports}},
                                          "Switch", 9607);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("switchType", "") == "asiair-plus-rk3568");
        EXPECT(cfg.value("devicePath", "") == "/dev/pwm-gpio-misc");
        EXPECT(cfg.value("pwmFrequencyHz", -1) == 50);
        EXPECT(cfg.contains("ports"));
        EXPECT(cfg["ports"][0].value("pwm", false) == true);
        // The gpioChip key belongs to the libgpiod variants only.
        EXPECT(!cfg.contains("gpioChip"));
        remove_device(router, "zwo", "switch", 9607);
    }
#endif

#ifdef ALPACACORE_ENABLE_QHY
    {
        // qhy / camera — cameraId is a STRING for QHY (char[32] ids).
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "qhy"},
                                           {"deviceType", "camera"},
                                           {"deviceNumber", 9608},
                                           {"cameraIndex", 1},
                                           {"cameraId", "QHY-TEST-1"}},
                                          "Camera", 9608);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("cameraIndex", -1) == 1);
        EXPECT(cfg.value("cameraId", "") == "QHY-TEST-1");
        remove_device(router, "qhy", "camera", 9608);
    }
#endif

#ifdef ALPACACORE_ENABLE_SVBONY
    {
        // svbony / camera
        const auto cfg = roundtrip_config(
            router, {{"vendor", "svbony"}, {"deviceType", "camera"}, {"deviceNumber", 9609}, {"cameraIndex", 2}},
            "Camera", 9609);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("cameraIndex", -1) == 2);
        remove_device(router, "svbony", "camera", 9609);
    }
#endif

#ifdef ALPACACORE_ENABLE_GPHOTO
    {
        // gphoto / camera
        const auto cfg = roundtrip_config(
            router, {{"vendor", "gphoto"}, {"deviceType", "camera"}, {"deviceNumber", 9620}, {"cameraIndex", 1}},
            "Camera", 9620);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("cameraIndex", -1) == 1);
        remove_device(router, "gphoto", "camera", 9620);
    }
#endif

#if defined(ALPACACORE_ENABLE_TOUPTEK) && defined(ALPACACORE_TOUPTEK_STELLAVITA)
    {
        // touptek / switch (StellaVita PowerBox) — field survival, not just
        // the existing no-crash test.
        nlohmann::json ports =
            nlohmann::json::array({{{"name", "Flat Panel"}, {"pwm", true}}, {{"name", "Camera"}, {"pwm", false}}});
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "touptek"},
                                           {"deviceType", "switch"},
                                           {"deviceNumber", 9610},
                                           {"switchType", "stellavita"},
                                           {"gpioChip", "/dev/gpiochip0"},
                                           {"pwmFrequencyHz", 100},
                                           {"ports", ports}},
                                          "Switch", 9610);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("switchType", "") == "stellavita");
        EXPECT(cfg.value("gpioChip", "") == "/dev/gpiochip0");
        EXPECT(cfg.value("pwmFrequencyHz", -1) == 100);
        EXPECT(cfg.contains("ports"));
        EXPECT(cfg["ports"][0].value("pwm", false) == true);
        remove_device(router, "touptek", "switch", 9610);
    }
#endif

#ifdef ALPACACORE_ENABLE_PLAYERONE
    {
        // playerone / camera — full round-trip (previous test was configure-only).
        const auto cfg = roundtrip_config(
            router, {{"vendor", "playerone"}, {"deviceType", "camera"}, {"deviceNumber", 9611}, {"cameraIndex", 3}},
            "Camera", 9611);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("cameraIndex", -1) == 3);
        remove_device(router, "playerone", "camera", 9611);
    }
    {
        // ioptron / camera (iCAM178M) — rebadged Player One camera routed to
        // the Player One driver; cameraIndex must survive the sanitizer.
        const auto cfg = roundtrip_config(
            router, {{"vendor", "ioptron"}, {"deviceType", "camera"}, {"deviceNumber", 9624}, {"cameraIndex", 2}},
            "Camera", 9624);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("vendor", "") == "ioptron");
        EXPECT(cfg.value("cameraIndex", -1) == 2);
        remove_device(router, "ioptron", "camera", 9624);
    }
    {
        // playerone / filterwheel
        const auto cfg =
            roundtrip_config(router,
                             {{"vendor", "playerone"},
                              {"deviceType", "filterwheel"},
                              {"deviceNumber", 9612},
                              {"filterwheelIndex", 1},
                              {"filterNames", nlohmann::json::array({"Lum", "Red", "Green", "Blue", "Ha"})}},
                             "FilterWheel", 9612);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("filterwheelIndex", -1) == 1);
        EXPECT(cfg.contains("filterNames"));
        EXPECT(cfg["filterNames"].size() == 5);
        remove_device(router, "playerone", "filterwheel", 9612);
    }
#endif

#ifdef ALPACACORE_ENABLE_GEMINI
    {
        // gemini / focuser
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "gemini"},
                                           {"deviceType", "focuser"},
                                           {"deviceNumber", 9613},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB7"},
                                           {"baudRate", 19200},
                                           {"focuserIndex", 1}},
                                          "Focuser", 9613);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB7");
        EXPECT(cfg.value("baudRate", -1) == 19200);
        EXPECT(cfg.value("focuserIndex", -1) == 1);
        remove_device(router, "gemini", "focuser", 9613);
    }
    {
        // gemini / covercalibrator (Flat Panel Cover Lite) — shares the vendor
        // config block with the focuser above; guards panelIndex persistence
        // through sanitize_device_config.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "gemini"},
                                           {"deviceType", "covercalibrator"},
                                           {"deviceNumber", 9618},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB8"},
                                           {"baudRate", 19200},
                                           {"panelIndex", 2}},
                                          "CoverCalibrator", 9618);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB8");
        EXPECT(cfg.value("baudRate", -1) == 19200);
        EXPECT(cfg.value("panelIndex", -1) == 2);
        remove_device(router, "gemini", "covercalibrator", 9618);
    }
    {
        // gemini / covercalibrator (Astro Automatic FlatPanel v2, motorized
        // cover) — same vendor+deviceType slot as the Lite case above,
        // distinguished by flatPanelModel; guards that field's persistence
        // through sanitize_device_config and that it actually selects the v2
        // driver (registered device count/type is the same either way, so
        // this only proves routing didn't reject the config).
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "gemini"},
                                           {"deviceType", "covercalibrator"},
                                           {"deviceNumber", 9619},
                                           {"flatPanelModel", "v2"},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB9"},
                                           {"baudRate", 19200},
                                           {"panelIndex", 3}},
                                          "CoverCalibrator", 9619);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("flatPanelModel", "") == "v2");
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB9");
        EXPECT(cfg.value("baudRate", -1) == 19200);
        EXPECT(cfg.value("panelIndex", -1) == 3);
        remove_device(router, "gemini", "covercalibrator", 9619);
    }
    {
        // gemini / covercalibrator (Motorized Flat Panel V3, "pro" firmware) —
        // third model on the same slot; guards flatPanelModel="pro" survives
        // sanitize_device_config and routing accepts it.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "gemini"},
                                           {"deviceType", "covercalibrator"},
                                           {"deviceNumber", 9620},
                                           {"flatPanelModel", "pro"},
                                           {"connectionType", "auto"},
                                           {"panelIndex", 1}},
                                          "CoverCalibrator", 9620);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("flatPanelModel", "") == "pro");
        EXPECT(cfg.value("connectionType", "") == "auto");
        EXPECT(cfg.value("panelIndex", -1) == 1);
        remove_device(router, "gemini", "covercalibrator", 9620);
    }
#endif

#ifdef ALPACACORE_ENABLE_ASTROASIS
    {
        // astroasis / focuser — explicit hidPath persists through
        // sanitize_device_config. (An empty hidPath instead falls back to
        // focuserIndex, which eagerly scans the USB bus at construction and
        // has no lazy no-hardware path to round-trip in this test.)
        const auto cfg = roundtrip_config(
            router,
            {{"vendor", "astroasis"}, {"deviceType", "focuser"}, {"deviceNumber", 9621}, {"hidPath", "/dev/hidraw3"}},
            "Focuser", 9621);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("hidPath", "") == "/dev/hidraw3");
        remove_device(router, "astroasis", "focuser", 9621);
    }
#endif

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
    {
        // wandererastro / rotator (WandererRotator Mini) — auto mode persists
        // rotatorIndex through sanitize_device_config.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "wandererastro"},
                                           {"deviceType", "rotator"},
                                           {"deviceNumber", 9619},
                                           {"connectionType", "auto"},
                                           {"rotatorIndex", 1}},
                                          "Rotator", 9619);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "auto");
        EXPECT(cfg.value("rotatorIndex", -1) == 1);
        remove_device(router, "wandererastro", "rotator", 9619);
    }

    {
        // wandererastro / switch (WandererBox Pro V3) — auto mode persists
        // switchType and boxIndex through sanitize_device_config.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "wandererastro"},
                                           {"deviceType", "switch"},
                                           {"deviceNumber", 9620},
                                           {"switchType", "wandererbox-pro-v3"},
                                           {"connectionType", "auto"},
                                           {"boxIndex", 1}},
                                          "Switch", 9620);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("switchType", "") == "wandererbox-pro-v3");
        EXPECT(cfg.value("connectionType", "") == "auto");
        EXPECT(cfg.value("boxIndex", -1) == 1);
        remove_device(router, "wandererastro", "switch", 9620);
    }
#endif

#ifdef ALPACACORE_ENABLE_GEMINI
    {
        // gemini / switch (Power & Data Hubs Advanced 3) -- auto mode persists
        // switchType and hubIndex through sanitize_device_config.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "gemini"},
                                           {"deviceType", "switch"},
                                           {"deviceNumber", 9625},
                                           {"switchType", "pdh-adv3"},
                                           {"connectionType", "auto"},
                                           {"hubIndex", 1}},
                                          "Switch", 9625);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("switchType", "") == "pdh-adv3");
        EXPECT(cfg.value("connectionType", "") == "auto");
        EXPECT(cfg.value("hubIndex", -1) == 1);
        remove_device(router, "gemini", "switch", 9625);
    }
#endif

#ifdef ALPACACORE_ENABLE_WEEWX
    {
        // weewx / observingconditions
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "weewx"},
                                           {"deviceType", "observingconditions"},
                                           {"deviceNumber", 9614},
                                           {"weewxUrl", "http://weewx.test:8998/current.json"},
                                           {"pollIntervalSeconds", 300},
                                           {"timeoutMs", 2500}},
                                          "ObservingConditions", 9614);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("weewxUrl", "") == "http://weewx.test:8998/current.json");
        EXPECT(cfg.value("pollIntervalSeconds", -1) == 300);
        EXPECT(cfg.value("timeoutMs", -1) == 2500);
        remove_device(router, "weewx", "observingconditions", 9614);
    }
#endif

#ifdef ALPACACORE_ENABLE_IOPTRON
    {
        // ioptron / telescope — asserts mountIndex survival: it is read by the
        // auto-detect registration path but was missing from the sanitizer
        // allowlist until issue #102 (saved index silently reverted to 0).
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "ioptron"},
                                           {"deviceType", "telescope"},
                                           {"deviceNumber", 9615},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB6"},
                                           {"baudRate", 115200},
                                           {"mountIndex", 1}},
                                          "Telescope", 9615);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB6");
        EXPECT(cfg.value("baudRate", -1) == 115200);
        EXPECT(cfg.value("mountIndex", -1) == 1);
        remove_device(router, "ioptron", "telescope", 9615);
    }
    {
        // ioptron / focuser (iEAF) — serial mode persists portPath (no
        // baudRate: the iEAF runs at a fixed 115200) and auto mode persists
        // focuserIndex through sanitize_device_config.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "ioptron"},
                                           {"deviceType", "focuser"},
                                           {"deviceNumber", 9622},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB7"},
                                           {"focuserIndex", 2},
                                           {"model", "iafs2"}},
                                          "Focuser", 9622);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("model", "") == "iafs2");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB7");
        EXPECT(cfg.value("focuserIndex", -1) == 2);
        remove_device(router, "ioptron", "focuser", 9622);
    }
    {
        // ioptron / filterwheel (iEFW) — serial mode persists portPath (no
        // baudRate: fixed 115200), auto mode persists filterwheelIndex, and
        // filterNames survive sanitize_device_config.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "ioptron"},
                                           {"deviceType", "filterwheel"},
                                           {"deviceNumber", 9623},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB8"},
                                           {"filterwheelIndex", 1},
                                           {"model", "iefw18"},
                                           {"filterNames", {"L", "R", "G", "B", "Ha"}}},
                                          "FilterWheel", 9623);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("model", "") == "iefw18");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB8");
        EXPECT(cfg.value("filterwheelIndex", -1) == 1);
        EXPECT(cfg.contains("filterNames") && cfg["filterNames"].size() == 5);
        remove_device(router, "ioptron", "filterwheel", 9623);
    }
#endif

#ifdef ALPACACORE_ENABLE_QHY
    {
        // qhy / focuser (Q-Focuser) — serial mode persists portPath (no
        // baudRate: fixed 9600) plus every connect-time setting through
        // sanitize_device_config; an unknown key is dropped.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "qhy"},
                                           {"deviceType", "focuser"},
                                           {"deviceNumber", 9640},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyACM3"},
                                           {"focuserIndex", 1},
                                           {"maxStep", 30000},
                                           {"reverse", true},
                                           {"speed", 4},
                                           {"holdForce", true},
                                           {"holdIhold", 6},
                                           {"holdIrun", 12},
                                           {"temperatureSource", "chip"},
                                           {"cameraIndex", 7}},
                                          "Focuser", 9640);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyACM3");
        EXPECT(cfg.value("focuserIndex", -1) == 1);
        EXPECT(cfg.value("maxStep", -1) == 30000);
        EXPECT(cfg.value("reverse", false) == true);
        EXPECT(cfg.value("speed", -1) == 4);
        EXPECT(cfg.value("holdForce", false) == true);
        EXPECT(cfg.value("holdIhold", -1) == 6);
        EXPECT(cfg.value("holdIrun", -1) == 12);
        EXPECT(cfg.value("temperatureSource", "") == "chip");
        EXPECT(!cfg.contains("cameraIndex"));
        remove_device(router, "qhy", "focuser", 9640);
    }
    {
        // qhy / focuser (Q-Focuser) — the router rejects out-of-range settings
        // with a specific message and a non-zero ErrorNumber, and registers
        // nothing. Covers the validation branch the valid round trip skips.
        const nlohmann::json bad = {{"vendor", "qhy"},
                                    {"deviceType", "focuser"},
                                    {"deviceNumber", 9641},
                                    {"connectionType", "serial"},
                                    {"portPath", "/dev/ttyACM4"},
                                    {"speed", 9}};
        const auto response = route_request(router, "POST", "/management/v1/configuredevice", bad.dump());
        const auto json = nlohmann::json::parse(response.body(), nullptr, false);
        EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        EXPECT(json.value("ErrorMessage", "").find("speed") != std::string::npos);
        // Nothing should have been registered at 9641.
        const auto listed = route_request(router, "GET", "/management/v1/configureddevices");
        const auto listed_json = nlohmann::json::parse(listed.body(), nullptr, false);
        bool present = false;
        if (!listed_json.is_discarded() && listed_json.contains("Value") && listed_json["Value"].is_array()) {
            for (const auto& entry : listed_json["Value"]) {
                if (entry.value("DeviceType", "") == "Focuser" && entry.value("DeviceNumber", -1) == 9641)
                    present = true;
            }
        }
        EXPECT(!present);
    }
    {
        // qhy / filterwheel, wheelType "cfw3-usb" (standalone QHYCFW3 on its
        // own serial port) — serial mode persists wheelType, connectionType,
        // portPath, filterwheelIndex and filterNames through
        // sanitize_device_config; the integrated wheel's cameraIndex/cameraId
        // are NOT kept for this backend (only the two fields its branch
        // reads), and an unknown key is dropped.
        const std::vector<std::string> names = {"L", "R", "G", "B", "Ha", "OIII", "SII"};
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "qhy"},
                                           {"deviceType", "filterwheel"},
                                           {"deviceNumber", 9642},
                                           {"wheelType", "cfw3-usb"},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB7"},
                                           {"filterwheelIndex", 1},
                                           {"filterNames", names},
                                           {"cameraIndex", 3},
                                           {"bogusKey", 1}},
                                          "FilterWheel", 9642);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("wheelType", "") == "cfw3-usb");
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB7");
        EXPECT(cfg.value("filterwheelIndex", -1) == 1);
        EXPECT(cfg["filterNames"] == names);
        EXPECT(!cfg.contains("cameraIndex"));
        EXPECT(!cfg.contains("bogusKey"));
        remove_device(router, "qhy", "filterwheel", 9642);
    }
    {
        // qhy / filterwheel — an unknown wheelType is refused with a message
        // naming the field, and nothing is registered.
        const nlohmann::json bad = {
            {"vendor", "qhy"},         {"deviceType", "filterwheel"}, {"deviceNumber", 9643},
            {"wheelType", "cfw2-usb"}, {"connectionType", "serial"},  {"portPath", "/dev/ttyUSB8"}};
        const auto response = route_request(router, "POST", "/management/v1/configuredevice", bad.dump());
        const auto json = nlohmann::json::parse(response.body(), nullptr, false);
        EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        EXPECT(json.value("ErrorMessage", "").find("wheelType") != std::string::npos);
        const auto listed = route_request(router, "GET", "/management/v1/configureddevices");
        const auto listed_json = nlohmann::json::parse(listed.body(), nullptr, false);
        bool present = false;
        if (!listed_json.is_discarded() && listed_json.contains("Value") && listed_json["Value"].is_array()) {
            for (const auto& entry : listed_json["Value"]) {
                if (entry.value("DeviceType", "") == "FilterWheel" && entry.value("DeviceNumber", -1) == 9643)
                    present = true;
            }
        }
        EXPECT(!present);
    }
    {
        // qhy / filterwheel, wheelType "cfw3-usb" — serial mode with no port
        // is refused rather than falling through to the CP210x probe.
        const nlohmann::json bad = {{"vendor", "qhy"},
                                    {"deviceType", "filterwheel"},
                                    {"deviceNumber", 9645},
                                    {"wheelType", "cfw3-usb"},
                                    {"connectionType", "serial"}};
        const auto response = route_request(router, "POST", "/management/v1/configuredevice", bad.dump());
        const auto json = nlohmann::json::parse(response.body(), nullptr, false);
        EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        EXPECT(json.value("ErrorMessage", "").find("portPath") != std::string::npos);
    }
    {
        // qhy / filterwheel, wheelType "cfw3-usb" — the connectionType
        // rejection is the sibling of the wheelType one above.
        const nlohmann::json bad = {
            {"vendor", "qhy"},         {"deviceType", "filterwheel"}, {"deviceNumber", 9644},
            {"wheelType", "cfw3-usb"}, {"connectionType", "network"}, {"portPath", "/dev/ttyUSB9"}};
        const auto response = route_request(router, "POST", "/management/v1/configuredevice", bad.dump());
        const auto json = nlohmann::json::parse(response.body(), nullptr, false);
        EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        EXPECT(json.value("ErrorMessage", "").find("connectionType") != std::string::npos);
        const auto listed = route_request(router, "GET", "/management/v1/configureddevices");
        const auto listed_json = nlohmann::json::parse(listed.body(), nullptr, false);
        bool present = false;
        if (!listed_json.is_discarded() && listed_json.contains("Value") && listed_json["Value"].is_array()) {
            for (const auto& entry : listed_json["Value"]) {
                if (entry.value("DeviceType", "") == "FilterWheel" && entry.value("DeviceNumber", -1) == 9644)
                    present = true;
            }
        }
        EXPECT(!present);
    }
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
    {
        // synscan / telescope — same mountIndex gap as ioptron; also the
        // synscanVersion discriminator must survive.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "synscan"},
                                           {"deviceType", "telescope"},
                                           {"deviceNumber", 9616},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB5"},
                                           {"baudRate", 9600},
                                           {"synscanVersion", "v4"},
                                           {"mountIndex", 2}},
                                          "Telescope", 9616);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB5");
        EXPECT(cfg.value("baudRate", -1) == 9600);
        EXPECT(cfg.value("synscanVersion", "") == "v4");
        EXPECT(cfg.value("mountIndex", -1) == 2);
        remove_device(router, "synscan", "telescope", 9616);
    }
#endif

#ifdef ALPACACORE_ENABLE_SKYWATCHER
    {
        // skywatcher / telescope (direct motor controller) — serial fields,
        // mountIndex, and the driver-owned site properties must all survive
        // the sanitize round-trip (the mount stores no site of its own).
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "skywatcher"},
                                           {"deviceType", "telescope"},
                                           {"deviceNumber", 9617},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB6"},
                                           {"baudRate", 9600},
                                           {"siteLatitude", 39.7392},
                                           {"siteLongitude", -104.9903},
                                           {"siteElevation", 1609.0},
                                           {"mountIndex", 1}},
                                          "Telescope", 9617);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB6");
        EXPECT(cfg.value("baudRate", -1) == 9600);
        EXPECT(cfg.value("siteLatitude", 0.0) == 39.7392);
        EXPECT(cfg.value("siteLongitude", 0.0) == -104.9903);
        EXPECT(cfg.value("siteElevation", 0.0) == 1609.0);
        EXPECT(cfg.value("mountIndex", -1) == 1);
        remove_device(router, "skywatcher", "telescope", 9617);
    }
    {
        // skywatcher / telescope network variant: host + udpPort (UDP 11880,
        // not tcpPort) must survive. Site coordinates are mandatory on this
        // vendor since issue #274, so they are supplied here too.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "skywatcher"},
                                           {"deviceType", "telescope"},
                                           {"deviceNumber", 9618},
                                           {"connectionType", "network"},
                                           {"host", "192.168.4.1"},
                                           {"udpPort", 11880},
                                           {"siteLatitude", -33.87},
                                           {"siteLongitude", 151.21}},
                                          "Telescope", 9618);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "network");
        EXPECT(cfg.value("host", "") == "192.168.4.1");
        EXPECT(cfg.value("udpPort", -1) == 11880);
        EXPECT(cfg.value("siteLatitude", 0.0) == -33.87);
        EXPECT(cfg.value("siteLongitude", 0.0) == 151.21);
        remove_device(router, "skywatcher", "telescope", 9618);
    }
    {
        // issue #274: configuredevice is a first-class REST API independent of
        // the web UI, and used to accept a skywatcher config with no
        // coordinates at all. Both would then collapse to 0.0 in the driver,
        // putting a southern rig on northern pointing math.
        // The message is asserted, not just "some error": a config rejected
        // for an unrelated reason (a renamed portPath key, say) would satisfy
        // ErrorNumber != 0 on its own, and this block is about the site rule.
        const auto reject = [&](const nlohmann::json& body) {
            const auto response = route_request(router, "POST", "/management/v1/configuredevice", body.dump());
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
            EXPECT(json.value("ErrorMessage", "").find("Site latitude and longitude are required") !=
                   std::string::npos);
        };
        nlohmann::json base = {{"vendor", "skywatcher"},     {"deviceType", "telescope"},  {"deviceNumber", 9619},
                               {"connectionType", "serial"}, {"portPath", "/dev/ttyUSB7"}, {"baudRate", 9600}};
        reject(base);  // neither coordinate
        nlohmann::json lat_only = base;
        lat_only["siteLatitude"] = -33.87;
        reject(lat_only);
        nlohmann::json lon_only = base;
        lon_only["siteLongitude"] = 151.21;
        reject(lon_only);

        // Null island is a real place: the rule is about presence, not value.
        nlohmann::json null_island = base;
        null_island["siteLatitude"] = 0.0;
        null_island["siteLongitude"] = 0.0;
        const auto ok = route_request(router, "POST", "/management/v1/configuredevice", null_island.dump());
        const auto ok_json = nlohmann::json::parse(ok.body(), nullptr, false);
        EXPECT(!ok_json.is_discarded() && ok_json.value("ErrorNumber", -1) == 0);
        remove_device(router, "skywatcher", "telescope", 9619);
    }
    {
        // issue #444: a site a client writes through the ASCOM setters is
        // persisted to the device's entry, so a location that only ever came
        // from a client (a phone's GPS through an app, gpsd) survives a
        // restart. Registered with null island, then each of the three
        // coordinates PUT through /api/v1, then read back through
        // configureddevices (the persisted entry's Config) and from the file.
        const auto site_config = [&](int number) {
            const auto listed = nlohmann::json::parse(
                route_request(router, "GET", "/management/v1/configureddevices").body(), nullptr, false);
            EXPECT(!listed.is_discarded() && listed.contains("Value") && listed["Value"].is_array());
            for (const auto& entry : listed["Value"]) {
                if (entry.value("DeviceType", "") == "Telescope" && entry.value("DeviceNumber", -1) == number) {
                    return entry.value("Config", nlohmann::json());
                }
            }
            return nlohmann::json();
        };
        const auto put_ok = [&](const std::string& path, const std::string& body) {
            const auto resp = route_request(router, "PUT", path, body);
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
        };
        const auto persisted_file_entry = [&](int number) {
            std::ifstream in(std::filesystem::path("config") / "registered_devices.json");
            const auto file = nlohmann::json::parse(in, nullptr, false);
            EXPECT(!file.is_discarded() && file.is_array());
            for (const auto& entry : file) {
                if (entry.value("deviceNumber", -1) == number) {
                    return entry;
                }
            }
            return nlohmann::json();
        };

        nlohmann::json learner = {{"vendor", "skywatcher"},     {"deviceType", "telescope"},  {"deviceNumber", 9640},
                                  {"connectionType", "serial"}, {"portPath", "/dev/ttyUSB9"}, {"baudRate", 9600},
                                  {"siteLatitude", 0.0},        {"siteLongitude", 0.0}};
        {
            const auto ok = route_request(router, "POST", "/management/v1/configuredevice", learner.dump());
            const auto ok_json = nlohmann::json::parse(ok.body(), nullptr, false);
            EXPECT(!ok_json.is_discarded() && ok_json.value("ErrorNumber", -1) == 0);
        }
        const std::string base = "/api/v1/telescope/9640";
        // #274 allows the setters before Connected, which is what makes a
        // client-supplied site usable at all on this vendor.
        put_ok(base + "/sitelatitude", "SiteLatitude=-33.87&ClientID=1&ClientTransactionID=1");
        put_ok(base + "/sitelongitude", "SiteLongitude=151.21&ClientID=1&ClientTransactionID=2");
        put_ok(base + "/siteelevation", "SiteElevation=58&ClientID=1&ClientTransactionID=3");
        {
            const auto cfg = site_config(9640);
            EXPECT(cfg.is_object() && std::fabs(cfg.value("siteLatitude", 0.0) - (-33.87)) < 1e-9);
            EXPECT(std::fabs(cfg.value("siteLongitude", 0.0) - 151.21) < 1e-9);
            EXPECT(std::fabs(cfg.value("siteElevation", 0.0) - 58.0) < 1e-9);
            // The other fields of the entry are untouched by the write-through.
            EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB9");
            EXPECT(cfg.value("baudRate", -1) == 9600);
            // And it reached the file, not just the in-memory list: that is
            // what a restart reads.
            const auto on_disk = persisted_file_entry(9640);
            EXPECT(on_disk.is_object() && std::fabs(on_disk.value("siteLatitude", 0.0) - (-33.87)) < 1e-9);
            EXPECT(std::fabs(on_disk.value("siteLongitude", 0.0) - 151.21) < 1e-9);
            EXPECT(std::fabs(on_disk.value("siteElevation", 0.0) - 58.0) < 1e-9);
        }
        // Unchanged: a client that re-sends its site on every connect (most
        // do) causes no file write. Delete the file, PUT the same value, and
        // the file is not recreated.
        {
            std::filesystem::remove(std::filesystem::path("config") / "registered_devices.json");
            put_ok(base + "/sitelatitude", "SiteLatitude=-33.87&ClientID=1&ClientTransactionID=4");
            EXPECT(!std::filesystem::exists(std::filesystem::path("config") / "registered_devices.json"));
            // A changed value writes it again, so the later file assertions
            // in this block still hold.
            put_ok(base + "/siteelevation", "SiteElevation=59&ClientID=1&ClientTransactionID=5");
            EXPECT(std::fabs(persisted_file_entry(9640).value("siteElevation", 0.0) - 59.0) < 1e-9);
            put_ok(base + "/siteelevation", "SiteElevation=58&ClientID=1&ClientTransactionID=6");
        }
        // The three site PUTs rewrite persisted configuration, so they carry
        // the cross-origin guard configuredevice and UTCDate (#401) carry: a
        // foreign Origin is refused with 403 before the value is parsed, the
        // driver or the file is touched; a same-origin write and one with no
        // Origin (native clients) go through. Same three-case shape as the
        // UTCDate block.
        {
            const auto send = [&](const std::string& member, const std::string& body, const std::string& origin) {
                std::ostringstream raw;
                raw << "PUT " << base << "/" << member << " HTTP/1.1\r\n"
                    << "Host: localhost\r\n";
                if (!origin.empty()) {
                    raw << "Origin: " << origin << "\r\n";
                }
                raw << "Content-Type: application/x-www-form-urlencoded\r\n"
                    << "Content-Length: " << body.size() << "\r\n\r\n"
                    << body;
                alpacahttp::Request request;
                EXPECT(request.parse(raw.str()));
                return router.route(request, 1);
            };
            const auto driver_value = [&](const std::string& member) {
                const auto json =
                    nlohmann::json::parse(route_request(router, "GET", base + "/" + member).body(), nullptr, false);
                return json.is_discarded() ? -1e9 : json.value("Value", -1e9);
            };
            EXPECT(send("sitelatitude", "SiteLatitude=10&ClientID=1", "http://evil.example").status_code() == 403);
            EXPECT(send("sitelongitude", "SiteLongitude=20&ClientID=1", "http://evil.example").status_code() == 403);
            EXPECT(send("siteelevation", "SiteElevation=30&ClientID=1", "http://evil.example").status_code() == 403);
            // Refused ahead of the parser too: a body that would fail
            // parse_double still gets the 403, not an InvalidValue.
            EXPECT(send("sitelatitude", "SiteLatitude=abc&ClientID=1", "http://evil.example").status_code() == 403);
            EXPECT(std::fabs(driver_value("sitelatitude") - (-33.87)) < 1e-9);
            EXPECT(std::fabs(driver_value("sitelongitude") - 151.21) < 1e-9);
            EXPECT(std::fabs(driver_value("siteelevation") - 58.0) < 1e-9);
            {
                const auto on_disk = persisted_file_entry(9640);
                EXPECT(on_disk.is_object() && std::fabs(on_disk.value("siteLatitude", 0.0) - (-33.87)) < 1e-9);
                EXPECT(std::fabs(on_disk.value("siteLongitude", 0.0) - 151.21) < 1e-9);
                EXPECT(std::fabs(on_disk.value("siteElevation", 0.0) - 58.0) < 1e-9);
            }
            EXPECT(send("siteelevation", "SiteElevation=61&ClientID=1", "http://localhost").status_code() != 403);
            EXPECT(std::fabs(driver_value("siteelevation") - 61.0) < 1e-9);
            EXPECT(send("siteelevation", "SiteElevation=58&ClientID=1", "").status_code() != 403);
            EXPECT(std::fabs(driver_value("siteelevation") - 58.0) < 1e-9);
            EXPECT(std::fabs(persisted_file_entry(9640).value("siteElevation", 0.0) - 58.0) < 1e-9);
        }
        // A NaN passes most drivers' range checks (both comparisons are
        // false), and nlohmann dumps a non-finite double as null, which the
        // next start reads as "absent". Since #574 the router rejects "nan"
        // with InvalidValue before the setter runs, so over HTTP it can no
        // longer reach the persistence hook; the surveyed value stays on disk.
        {
            const auto resp = route_request(router, "PUT", base + "/sitelatitude", "SiteLatitude=nan&ClientID=1");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) == 0x401);
            const auto on_disk = persisted_file_entry(9640);
            EXPECT(on_disk.is_object() && on_disk.contains("siteLatitude") && on_disk["siteLatitude"].is_number());
            EXPECT(std::fabs(on_disk.value("siteLatitude", 0.0) - (-33.87)) < 1e-9);
            EXPECT(std::fabs(site_config(9640).value("siteLatitude", 0.0) - (-33.87)) < 1e-9);
            // Restore the driver's own value for the blocks that follow.
            put_ok(base + "/sitelatitude", "SiteLatitude=-33.87&ClientID=1&ClientTransactionID=7");
        }
        // A value the driver refuses is not persisted either: the hook runs
        // after the setter, so the throw never reaches it.
        {
            const auto resp = route_request(router, "PUT", base + "/sitelatitude", "SiteLatitude=95&ClientID=1");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
            EXPECT(std::fabs(site_config(9640).value("siteLatitude", 0.0) - (-33.87)) < 1e-9);
            EXPECT(std::fabs(persisted_file_entry(9640).value("siteLatitude", 0.0) - (-33.87)) < 1e-9);
        }
        remove_device(router, "skywatcher", "telescope", 9640);

        // The opt-out: learnSiteFromClient=false keeps a surveyed position
        // against a client's GPS. The flag itself round-trips through
        // sanitize_device_config, the setter still succeeds (the driver takes
        // the value for this session), and the persisted entry is unchanged.
        nlohmann::json surveyed = learner;
        surveyed["deviceNumber"] = 9641;
        surveyed["siteLatitude"] = 39.7392;
        surveyed["siteLongitude"] = -104.9903;
        surveyed["learnSiteFromClient"] = false;
        {
            const auto ok = route_request(router, "POST", "/management/v1/configuredevice", surveyed.dump());
            const auto ok_json = nlohmann::json::parse(ok.body(), nullptr, false);
            EXPECT(!ok_json.is_discarded() && ok_json.value("ErrorNumber", -1) == 0);
        }
        put_ok("/api/v1/telescope/9641/sitelatitude", "SiteLatitude=-33.87&ClientID=1");
        put_ok("/api/v1/telescope/9641/siteelevation", "SiteElevation=58&ClientID=1");
        {
            const auto cfg = site_config(9641);
            EXPECT(cfg.is_object() && cfg.value("learnSiteFromClient", true) == false);
            EXPECT(std::fabs(cfg.value("siteLatitude", 0.0) - 39.7392) < 1e-9);
            EXPECT(!cfg.contains("siteElevation"));
            const auto resp = route_request(router, "GET", "/api/v1/telescope/9641/sitelatitude?ClientID=1");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && std::fabs(json.value("Value", 0.0) - (-33.87)) < 1e-9);
        }
        remove_device(router, "skywatcher", "telescope", 9641);

        // A non-boolean flag (a shell/jq-built config sends "false" or 0) is
        // refused at the POST with the field named, the #388 rule, so it
        // never reaches the file where a later value() read would throw.
        nlohmann::json typod = learner;
        typod["deviceNumber"] = 9642;
        typod["learnSiteFromClient"] = "false";
        {
            const auto resp = route_request(router, "POST", "/management/v1/configuredevice", typod.dump());
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
            EXPECT(json.value("ErrorMessage", "").find("learnSiteFromClient") != std::string::npos);
            EXPECT(site_config(9642).is_null());
        }
    }
    {
        // issue #444, the hand-edited half: a persisted entry whose
        // learnSiteFromClient is the string "false" (no API validation ever
        // saw it) must not turn every site PUT into an error. The setter
        // succeeds and the site is learned: only a boolean false is the
        // opt-out. Same one-entry-file + second-Router shape as the #274
        // block below, for the reasons given there.
        const std::filesystem::path persisted = std::filesystem::path("config") / "registered_devices.json";
        std::string original;
        if (std::filesystem::exists(persisted)) {
            std::ifstream in(persisted);
            original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        nlohmann::json entries = nlohmann::json::array();
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9643},
                           {"connectionType", "serial"},
                           {"portPath", "/dev/ttyUSB8"},
                           {"baudRate", 9600},
                           {"siteLatitude", 0.0},
                           {"siteLongitude", 0.0},
                           {"learnSiteFromClient", "false"}});
        std::filesystem::create_directories(persisted.parent_path());
        {
            std::ofstream out(persisted, std::ios::trunc);
            out << entries.dump();
        }
        alpacahttp::Router startup_router;
        const auto put =
            nlohmann::json::parse(route_request(startup_router, "PUT", "/api/v1/telescope/9643/sitelatitude",
                                                "SiteLatitude=-33.87&ClientID=1")
                                      .body(),
                                  nullptr, false);
        const auto listed = nlohmann::json::parse(
            route_request(startup_router, "GET", "/management/v1/configureddevices").body(), nullptr, false);
        const auto restore_original = [&] {
            std::ofstream restore(persisted, std::ios::trunc);
            restore << (original.empty() ? std::string("[]") : original);
        };
        restore_original();
        remove_device(startup_router, "skywatcher", "telescope", 9643);
        restore_original();

        EXPECT(!put.is_discarded() && put.value("ErrorNumber", -1) == 0);
        EXPECT(!listed.is_discarded() && listed.contains("Value") && listed["Value"].is_array());
        bool learned = false;
        for (const auto& entry : listed["Value"]) {
            if (entry.value("DeviceType", "") == "Telescope" && entry.value("DeviceNumber", -1) == 9643) {
                const auto cfg = entry.value("Config", nlohmann::json());
                learned = cfg.is_object() && std::fabs(cfg.value("siteLatitude", 0.0) - (-33.87)) < 1e-9;
            }
        }
        EXPECT(learned);
    }
    {
        // issue #444, the concurrency half: site PUTs run on the worker pool,
        // so two clients can reach save_persisted_devices() at once. A
        // truncate-in-place write from two threads interleaves and leaves the
        // file as one dump's head plus the other's tail, invalid JSON, and
        // the next start then loads NO devices. A pair of telescopes, a thread each,
        // each pushing an alternating coordinate; the file must parse after
        // every write, so it is checked from a third thread throughout and
        // once more at the end.
        nlohmann::json a = {{"vendor", "skywatcher"},     {"deviceType", "telescope"},  {"deviceNumber", 9653},
                            {"connectionType", "serial"}, {"portPath", "/dev/ttyUSB9"}, {"baudRate", 9600},
                            {"siteLatitude", 0.0},        {"siteLongitude", 0.0}};
        nlohmann::json b = a;
        b["deviceNumber"] = 9654;
        for (const auto& cfg : {a, b}) {
            const auto ok = route_request(router, "POST", "/management/v1/configuredevice", cfg.dump());
            const auto ok_json = nlohmann::json::parse(ok.body(), nullptr, false);
            EXPECT(!ok_json.is_discarded() && ok_json.value("ErrorNumber", -1) == 0);
        }
        const std::filesystem::path persisted = std::filesystem::path("config") / "registered_devices.json";
        std::atomic<bool> done{false};
        std::atomic<int> unreadable{0};
        std::atomic<int> failed_puts{0};
        const auto pusher = [&](int number, const char* name, double base) {
            for (int i = 0; i < 150; ++i) {
                // Every value differs from the last, so every PUT is a write.
                const double v = base + 0.001 * (i + 1);
                const auto resp = route_request(
                    router, "PUT", "/api/v1/telescope/" + std::to_string(number) + "/" + name + "?ClientID=1",
                    std::string(name == std::string("sitelatitude") ? "SiteLatitude" : "SiteElevation") + "=" +
                        std::to_string(v) + "&ClientID=1");
                const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
                if (json.is_discarded() || json.value("ErrorNumber", -1) != 0) {
                    ++failed_puts;
                }
            }
        };
        std::thread reader([&] {
            while (!done) {
                std::ifstream in(persisted);
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (!text.empty() && nlohmann::json::parse(text, nullptr, false).is_discarded()) {
                    ++unreadable;
                }
            }
        });
        std::thread t1(pusher, 9653, "sitelatitude", -33.0);
        std::thread t2(pusher, 9654, "siteelevation", 100.0);
        t1.join();
        t2.join();
        done = true;
        reader.join();
        EXPECT(failed_puts == 0);
        EXPECT(unreadable == 0);
        {
            std::ifstream in(persisted);
            const auto file = nlohmann::json::parse(in, nullptr, false);
            EXPECT(!file.is_discarded() && file.is_array());
        }
        remove_device(router, "skywatcher", "telescope", 9653);
        remove_device(router, "skywatcher", "telescope", 9654);
    }
    {
        // issue #444, the malformed-neighbour half: a hand-edited entry whose
        // deviceNumber is the string "3" stays in the persisted list (only
        // its registration fails), and a site PUT to a DIFFERENT, valid
        // telescope walks past it. The hook must read that entry through
        // typed guards, never value(), or the PUT to the good device fails.
        const std::filesystem::path persisted = std::filesystem::path("config") / "registered_devices.json";
        std::string original;
        if (std::filesystem::exists(persisted)) {
            std::ifstream in(persisted);
            original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        nlohmann::json entries = nlohmann::json::array();
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", "3"},
                           {"connectionType", "serial"},
                           {"portPath", "/dev/ttyUSB7"},
                           {"baudRate", 9600}});
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9652},
                           {"connectionType", "serial"},
                           {"portPath", "/dev/ttyUSB8"},
                           {"baudRate", 9600},
                           {"siteLatitude", 0.0},
                           {"siteLongitude", 0.0}});
        std::filesystem::create_directories(persisted.parent_path());
        {
            std::ofstream out(persisted, std::ios::trunc);
            out << entries.dump();
        }
        alpacahttp::Router startup_router;
        const auto put =
            nlohmann::json::parse(route_request(startup_router, "PUT", "/api/v1/telescope/9652/sitelatitude",
                                                "SiteLatitude=-33.87&ClientID=1")
                                      .body(),
                                  nullptr, false);
        const auto listed = nlohmann::json::parse(
            route_request(startup_router, "GET", "/management/v1/configureddevices").body(), nullptr, false);
        const auto restore_original = [&] {
            std::ofstream restore(persisted, std::ios::trunc);
            restore << (original.empty() ? std::string("[]") : original);
        };
        restore_original();
        remove_device(startup_router, "skywatcher", "telescope", 9652);
        restore_original();

        EXPECT(!put.is_discarded() && put.value("ErrorNumber", -1) == 0);
        EXPECT(!listed.is_discarded() && listed.contains("Value") && listed["Value"].is_array());
        bool learned = false;
        for (const auto& entry : listed["Value"]) {
            if (entry.value("DeviceType", "") == "Telescope" && entry.value("DeviceNumber", -1) == 9652) {
                const auto cfg = entry.value("Config", nlohmann::json());
                learned = cfg.is_object() && std::fabs(cfg.value("siteLatitude", 0.0) - (-33.87)) < 1e-9;
            }
        }
        EXPECT(learned);
    }
    {
        // The persisted_key() readers pin three behaviours the #444 refactor
        // carries, each on a hand-edited file loaded by a fresh Router:
        // (1) deviceType compares case-insensitively, so an entry stored as
        //     "Telescope" is matched by a lowercase removedevice and does not
        //     come back after a restart; (2) an entry whose registration
        //     failed is listed with its DeviceType lowercased; (3) a
        //     deviceNumber written as 9656.0 registers as device 9656 (the
        //     int config reader converts any number), so the persisted walk
        //     must match it the same way: it is listed, and removedevice
        //     takes it out of the file too, not only out of the registry.
        const std::filesystem::path persisted = std::filesystem::path("config") / "registered_devices.json";
        std::string original;
        if (std::filesystem::exists(persisted)) {
            std::ifstream in(persisted);
            original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        nlohmann::json entries = nlohmann::json::array();
        // Registration fails (unknown vendor), so the entry is listed from
        // the persisted snapshot, not from a live driver.
        entries.push_back({{"vendor", "no-such-vendor"}, {"deviceType", "Telescope"}, {"deviceNumber", 9655}});
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9656.0},
                           {"connectionType", "serial"},
                           {"portPath", "/dev/ttyUSB9"},
                           {"baudRate", 9600}});
        std::filesystem::create_directories(persisted.parent_path());
        {
            std::ofstream out(persisted, std::ios::trunc);
            out << entries.dump();
        }
        alpacahttp::Router startup_router;
        const auto listed = nlohmann::json::parse(
            route_request(startup_router, "GET", "/management/v1/configureddevices").body(), nullptr, false);
        bool listed_lowercase = false;
        bool float_listed = false;
        if (!listed.is_discarded() && listed.contains("Value") && listed["Value"].is_array()) {
            for (const auto& entry : listed["Value"]) {
                if (entry.value("DeviceNumber", -1) == 9655) {
                    listed_lowercase = entry.value("DeviceType", "") == "telescope";
                }
                if (entry.value("DeviceNumber", -1) == 9656) {
                    float_listed = true;
                }
            }
        }
        const auto remove_upper = nlohmann::json::parse(
            route_request(
                startup_router, "POST", "/management/v1/removedevice",
                nlohmann::json({{"vendor", "no-such-vendor"}, {"deviceType", "telescope"}, {"deviceNumber", 9655}})
                    .dump())
                .body(),
            nullptr, false);
        (void)route_request(
            startup_router, "POST", "/management/v1/removedevice",
            nlohmann::json({{"vendor", "skywatcher"}, {"deviceType", "telescope"}, {"deviceNumber", 9656}}).dump());
        nlohmann::json on_disk;
        {
            std::ifstream in(persisted);
            on_disk = nlohmann::json::parse(in, nullptr, false);
        }
        {
            std::ofstream restore(persisted, std::ios::trunc);
            restore << (original.empty() ? std::string("[]") : original);
        }
        EXPECT(listed_lowercase);
        EXPECT(float_listed);
        EXPECT(!remove_upper.is_discarded() && remove_upper.value("ErrorNumber", -1) == 0);
        bool upper_gone = true;
        bool float_gone = true;
        if (on_disk.is_array()) {
            for (const auto& entry : on_disk) {
                if (entry.value("vendor", "") == "no-such-vendor") {
                    upper_gone = false;
                }
                if (entry.contains("deviceNumber") && entry["deviceNumber"].is_number_float()) {
                    float_gone = false;
                }
            }
        }
        EXPECT(upper_gone);
        EXPECT(float_gone);
    }
    {
        // issue #274, the other half: a config already on disk cannot be
        // corrected by its caller. Dropping it at startup would keep it out of
        // the device registry, and configureddevices -- the web UI's only
        // source of devices -- would then not list it, leaving the operator no
        // way to edit the very entry that is at fault. A persisted
        // skywatcher entry with no coordinates must still be registered and
        // still be listed; the driver's connect-time guard is what refuses it.
        const std::filesystem::path persisted = std::filesystem::path("config") / "registered_devices.json";
        std::string original;
        if (std::filesystem::exists(persisted)) {
            std::ifstream in(persisted);
            original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        // Only this entry, rather than appending to whatever is on disk: the
        // second Router below re-registers EVERY entry in the file and builds
        // that vendor's driver, and some vendors touch hardware eagerly (the
        // astroasis by-index path .github/instructions/astroasis.instructions.md warns about). Appending would make
        // this case depend on every earlier block having removed what it added,
        // which nothing enforces. The original contents are restored below.
        nlohmann::json entries = nlohmann::json::array();
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9630},
                           {"connectionType", "serial"},
                           {"portPath", "/dev/ttyUSB8"},
                           {"baudRate", 9600}});
        std::filesystem::create_directories(persisted.parent_path());
        {
            std::ofstream out(persisted, std::ios::trunc);
            out << entries.dump();
        }

        // A second Router: load_persisted_devices() is one-shot per instance,
        // so the startup path only runs on an instance that has not read the
        // file yet.
        alpacahttp::Router startup_router;
        const auto listed_json = nlohmann::json::parse(
            route_request(startup_router, "GET", "/management/v1/configureddevices").body(), nullptr, false);

        // Put the file back BEFORE anything that can abort, and not from a
        // destructor: EXPECT is abort(), which neither unwinds the stack nor
        // runs a scope guard. That includes remove_device() below, which is
        // itself an EXPECT -- and it is exactly the call that fails in the
        // regression this block guards against, since a device that was never
        // registered cannot be removed.
        const auto restore_original = [&] {
            std::ofstream restore(persisted, std::ios::trunc);
            restore << (original.empty() ? std::string("[]") : original);
        };
        restore_original();

        // Unregister from the process-wide DeviceRegistry so later blocks do
        // not see 9630. remove_device() saves from THIS router's in-memory
        // list, which was loaded from the synthetic one-entry file, so it
        // writes "[]" over the restore above; restore once more afterwards so
        // the file really is the original when this block ends (#408).
        remove_device(startup_router, "skywatcher", "telescope", 9630);
        restore_original();

        EXPECT(!listed_json.is_discarded() && listed_json.contains("Value") && listed_json["Value"].is_array());
        bool found = false;
        for (const auto& entry : listed_json["Value"]) {
            if (entry.value("DeviceType", "") == "Telescope" && entry.value("DeviceNumber", -1) == 9630) {
                found = true;
            }
        }
        EXPECT(found);
    }
    {
        // issue #398, the persisted half: an out-of-range coordinate already on
        // disk is WARNED about and CLEARED, not rejected and not applied. The
        // API half (reject with a message) is covered above; this arm has the
        // opposite shape on purpose (#353): dropping the entry would keep it
        // out of configureddevices, which is the web UI's only source of
        // devices, leaving the operator no way to edit the entry at fault.
        //
        // The assertion that matters is the CLEAR. Replacing the warn-and-skip
        // with `*field.out = value;` -- which restores the #398 bug for every
        // device already on disk, the larger population -- still registers the
        // device, so registration alone proves nothing. SkyWatcher's
        // get_site_latitude() just returns the stored value under its mutex,
        // with no connection check, so the value itself is observable here.
        const std::filesystem::path persisted = std::filesystem::path("config") / "registered_devices.json";
        std::string original;
        if (std::filesystem::exists(persisted)) {
            std::ifstream in(persisted);
            original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        nlohmann::json entries = nlohmann::json::array();
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9633},
                           {"connectionType", "serial"},
                           {"portPath", "/dev/ttyUSB8"},
                           {"baudRate", 9600},
                           {"siteLatitude", 200.0},
                           {"siteLongitude", 172.6}});
        std::filesystem::create_directories(persisted.parent_path());
        {
            std::ofstream out(persisted, std::ios::trunc);
            out << entries.dump();
        }

        alpacahttp::Router startup_router;
        const auto lat_json = nlohmann::json::parse(
            route_request(startup_router, "GET", "/api/v1/telescope/9633/sitelatitude").body(), nullptr, false);
        const auto lon_json = nlohmann::json::parse(
            route_request(startup_router, "GET", "/api/v1/telescope/9633/sitelongitude").body(), nullptr, false);

        const auto restore_original = [&] {
            std::ofstream restore(persisted, std::ios::trunc);
            restore << (original.empty() ? std::string("[]") : original);
        };
        restore_original();
        remove_device(startup_router, "skywatcher", "telescope", 9633);
        restore_original();

        // Registered despite the bad coordinate, and the bad coordinate did
        // NOT reach the driver.
        EXPECT(!lat_json.is_discarded() && lat_json.value("ErrorNumber", -1) == 0);
        EXPECT(lat_json["Value"].get<double>() != 200.0);
        // The valid sibling on the same entry is still applied -- the skip is
        // per field, not per entry.
        EXPECT(!lon_json.is_discarded() && lon_json.value("ErrorNumber", -1) == 0);
        EXPECT(std::abs(lon_json["Value"].get<double>() - 172.6) < 1e-9);
    }
    {
        // issue #408 (second item): the startup WARN for a half-configured
        // persisted entry names the half that is missing. Two entries, one
        // with only a latitude and one with only a longitude, loaded by a
        // fresh Router while the log sink is captured; the text is pinned so
        // swapping the two arms cannot pass.
        const std::filesystem::path persisted = std::filesystem::path("config") / "registered_devices.json";
        std::string original;
        if (std::filesystem::exists(persisted)) {
            std::ifstream in(persisted);
            original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        nlohmann::json entries = nlohmann::json::array();
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9631},
                           {"connectionType", "serial"},
                           {"portPath", "/dev/ttyUSB8"},
                           {"baudRate", 9600},
                           {"siteLatitude", 39.7392}});
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9632},
                           {"connectionType", "serial"},
                           {"portPath", "/dev/ttyUSB9"},
                           {"baudRate", 9600},
                           {"siteLongitude", -104.9903}});
        std::filesystem::create_directories(persisted.parent_path());
        {
            std::ofstream out(persisted, std::ios::trunc);
            out << entries.dump();
        }
        std::vector<std::string> warnings;
        std::mutex warnings_mutex;
        auto previous_sink = alpacacore::logging::get_log_sink();
        alpacacore::logging::set_log_sink(
            [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn) {
                    std::lock_guard<std::mutex> lock(warnings_mutex);
                    warnings.emplace_back(message);
                }
            });
        alpacahttp::Router half_router;
        static_cast<void>(route_request(half_router, "GET", "/management/v1/configureddevices"));
        alpacacore::logging::set_log_sink(previous_sink);
        const auto restore_original = [&] {
            std::ofstream restore(persisted, std::ios::trunc);
            restore << (original.empty() ? std::string("[]") : original);
        };
        restore_original();
        remove_device(half_router, "skywatcher", "telescope", 9631);
        remove_device(half_router, "skywatcher", "telescope", 9632);
        restore_original();
        bool lat_only = false;
        bool lon_only = false;
        for (const auto& w : warnings) {
            if (w.find("telescope 9631 has no site longitude and will refuse to connect") != std::string::npos) {
                lat_only = true;
            }
            if (w.find("telescope 9632 has no site latitude and will refuse to connect") != std::string::npos) {
                lon_only = true;
            }
        }
        EXPECT(lat_only);
        EXPECT(lon_only);
    }
    {
        // Issue #380: the portPath / host / connectionType checks follow the
        // same source rule as the site-coordinate check above. All three used
        // to `return false` regardless of source, so a persisted entry with an
        // empty portPath vanished from the web UI at startup -- the exact
        // failure the rule exists to prevent, and the one an operator is most
        // likely to hit, since the port path is what goes wrong after a USB
        // device is renamed.
        const std::filesystem::path persisted = std::filesystem::path("config") / "registered_devices.json";
        std::string original;
        if (std::filesystem::exists(persisted)) {
            std::ifstream in(persisted);
            original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        // One entry per check. Site coordinates are present throughout so a
        // failure here cannot be the #274 rule firing instead.
        nlohmann::json entries = nlohmann::json::array();
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9640},
                           {"connectionType", "serial"},
                           {"portPath", ""},
                           {"baudRate", 9600},
                           {"siteLatitude", 39.7392},
                           {"siteLongitude", -104.9903}});
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9641},
                           {"connectionType", "network"},
                           {"host", ""},
                           {"siteLatitude", 39.7392},
                           {"siteLongitude", -104.9903}});
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9642},
                           {"connectionType", "carrier-pigeon"},
                           {"portPath", "/dev/ttyUSB8"},
                           {"baudRate", 9600},
                           {"siteLatitude", 39.7392},
                           {"siteLongitude", -104.9903}});
#ifdef ALPACACORE_ENABLE_ZWO
        // Review finding: the ZWO branch tests a bare `conn_type == "auto"`,
        // not `|| conn_type.empty()`, so an entry with no connectionType key
        // falls to its else and used to be dropped regardless of source -- the
        // one branch a blanket "empty is always valid" rule in the helper
        // would have left unfixed. The off-UI path: hand-edited, or written by
        // a non-web-UI client, since the form always sets the field.
        entries.push_back({{"vendor", "zwo"}, {"deviceType", "telescope"}, {"deviceNumber", 9644}});
#endif
#ifdef ALPACACORE_ENABLE_BISQUE
        // Review finding: the bisque branch has no connectionType at all --
        // it is TCP-only -- and its host check was the one telescope branch
        // still doing an inline `return false`, so a persisted entry with an
        // empty host was dropped at startup while the other six were kept.
        entries.push_back({{"vendor", "bisque"}, {"deviceType", "telescope"}, {"deviceNumber", 9646}, {"host", ""}});
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
        // OnStep is serial-only, so its valid list is shorter: a persisted
        // "network" is unrecognised HERE even though it is valid for the other
        // four, and normalises to serial rather than dropping the device.
        entries.push_back({{"vendor", "onstep"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9645},
                           {"connectionType", "network"},
                           {"host", "192.168.1.50"}});
#endif
        std::filesystem::create_directories(persisted.parent_path());
        {
            std::ofstream out(persisted, std::ios::trunc);
            out << entries.dump();
        }

        std::vector<std::string> warnings;
        std::mutex warnings_mutex;
        auto previous_sink = alpacacore::logging::get_log_sink();
        alpacacore::logging::set_log_sink(
            [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn) {
                    std::lock_guard<std::mutex> lock(warnings_mutex);
                    warnings.emplace_back(message);
                }
            });
        alpacahttp::Router startup_router;
        const auto listed_json = nlohmann::json::parse(
            route_request(startup_router, "GET", "/management/v1/configureddevices").body(), nullptr, false);
        alpacacore::logging::set_log_sink(previous_sink);

        // Restore before anything that can abort, and not from a destructor:
        // EXPECT is abort(), which neither unwinds nor runs a scope guard.
        const auto restore_original = [&] {
            std::ofstream restore(persisted, std::ios::trunc);
            restore << (original.empty() ? std::string("[]") : original);
        };
        restore_original();
        remove_device(startup_router, "skywatcher", "telescope", 9640);
        remove_device(startup_router, "skywatcher", "telescope", 9641);
        remove_device(startup_router, "skywatcher", "telescope", 9642);
#ifdef ALPACACORE_ENABLE_ZWO
        remove_device(startup_router, "zwo", "telescope", 9644);
#endif
#ifdef ALPACACORE_ENABLE_BISQUE
        remove_device(startup_router, "bisque", "telescope", 9646);
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
        remove_device(startup_router, "onstep", "telescope", 9645);
#endif
        restore_original();

        EXPECT(!listed_json.is_discarded() && listed_json.contains("Value") && listed_json["Value"].is_array());
        std::vector<int> expected_listed = {9640, 9641, 9642};
#ifdef ALPACACORE_ENABLE_ZWO
        expected_listed.push_back(9644);
#endif
#ifdef ALPACACORE_ENABLE_BISQUE
        expected_listed.push_back(9646);
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
        expected_listed.push_back(9645);
#endif
        for (int device_number : expected_listed) {
            bool found = false;
            for (const auto& entry : listed_json["Value"]) {
                if (entry.value("DeviceType", "") == "Telescope" && entry.value("DeviceNumber", -1) == device_number) {
                    found = true;
                }
            }
            EXPECT(found);
        }

        // Each registration is accompanied by a WARN that names the reason, so
        // "registered anyway" never means "registered silently". The
        // connection-type line is distinct: that entry is not merely warned
        // about, it is normalised to serial, and the log has to say so or the
        // operator cannot explain the connect error they then get.
        bool warned_port = false;
        bool warned_host = false;
        bool warned_conn_type = false;
        for (const auto& w : warnings) {
            if (w.find("telescope 9640") != std::string::npos &&
                w.find("Serial port path is required") != std::string::npos) {
                warned_port = true;
            }
            if (w.find("telescope 9641") != std::string::npos &&
                w.find("Host IP address is required") != std::string::npos) {
                warned_host = true;
            }
            // The fallback VALUE is the judgement this whole change rests on:
            // "serial" makes the connect fail on the port path, while "auto"
            // would auto-probe and attach to whatever mount answers. Nothing
            // pinned it, so flipping the helper to "auto" kept the suite
            // green -- assert the fragment, not just that a WARN happened.
            if (w.find("telescope 9642") != std::string::npos &&
                w.find("has connectionType \"carrier-pigeon\"") != std::string::npos &&
                w.find("treating it as \"serial\"") != std::string::npos) {
                warned_conn_type = true;
            }
        }
        EXPECT(warned_port);
        EXPECT(warned_host);
        EXPECT(warned_conn_type);

#ifdef ALPACACORE_ENABLE_BISQUE
        bool warned_bisque_host = false;
#endif
#ifdef ALPACACORE_ENABLE_ZWO
        bool warned_zwo_empty = false;
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
        bool warned_onstep_network = false;
#endif
        for (const auto& w : warnings) {
#ifdef ALPACACORE_ENABLE_BISQUE
            if (w.find("telescope 9646") != std::string::npos &&
                w.find("Host is required for Bisque/TheSkyX connection") != std::string::npos) {
                warned_bisque_host = true;
            }
#endif
#ifdef ALPACACORE_ENABLE_ZWO
            // Match the normalisation line's own distinctive wording, not a
            // bare "serial": the sibling port-path WARN only fails to match
            // that because it capitalises "Serial", which is a coincidence of
            // wording rather than something this case should rest on.
            if (w.find("telescope 9644") != std::string::npos && w.find("has connectionType") != std::string::npos) {
                warned_zwo_empty = true;
            }
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
            if (w.find("telescope 9645") != std::string::npos &&
                w.find("has connectionType \"network\"") != std::string::npos) {
                warned_onstep_network = true;
            }
#endif
        }
#ifdef ALPACACORE_ENABLE_BISQUE
        EXPECT(warned_bisque_host);
#endif
#ifdef ALPACACORE_ENABLE_ZWO
        EXPECT(warned_zwo_empty);
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
        EXPECT(warned_onstep_network);
#endif
    }
    {
        // The other half of the rule, unchanged: the same three configs are
        // still rejected outright when they arrive through the API, where the
        // caller can fix them and nothing has been written to disk yet.
        alpacahttp::Router router;
        const nlohmann::json base = {{"vendor", "skywatcher"},
                                     {"deviceType", "telescope"},
                                     {"deviceNumber", 9643},
                                     {"siteLatitude", 39.7392},
                                     {"siteLongitude", -104.9903}};
        const auto reject = [&router](nlohmann::json body, const std::string& expected) {
            const auto response = route_request(router, "POST", "/management/v1/configuredevice", body.dump());
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded());
            EXPECT(json.value("ErrorNumber", 0) != 0);
            EXPECT(json.value("ErrorMessage", "").find(expected) != std::string::npos);
        };

        nlohmann::json no_port = base;
        no_port["connectionType"] = "serial";
        no_port["portPath"] = "";
        reject(no_port, "Serial port path is required");

        nlohmann::json no_host = base;
        no_host["connectionType"] = "network";
        no_host["host"] = "";
        reject(no_host, "Host IP address is required");

        nlohmann::json bad_type = base;
        bad_type["connectionType"] = "carrier-pigeon";
        bad_type["portPath"] = "/dev/ttyUSB8";
        reject(bad_type, "Invalid connection type");

#ifdef ALPACACORE_ENABLE_BISQUE
        // Bisque has no connectionType and no portPath: its host is the whole
        // config, so it gets its own API case rather than a variant of base.
        reject({{"vendor", "bisque"}, {"deviceType", "telescope"}, {"deviceNumber", 9647}, {"host", ""}},
               "Host is required for Bisque/TheSkyX connection");
#endif
    }

#endif

#ifdef ALPACACORE_ENABLE_ONSTEP
    {
        // onstep / telescope — same mountIndex allowlist gap class as
        // ioptron/synscan above; also asserts no "network" fields leak
        // through (OnStep is serial-only for end users in this project).
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "onstep"},
                                           {"deviceType", "telescope"},
                                           {"deviceNumber", 9622},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyACM0"},
                                           {"baudRate", 9600},
                                           {"mountIndex", 1}},
                                          "Telescope", 9622);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyACM0");
        EXPECT(cfg.value("baudRate", -1) == 9600);
        EXPECT(cfg.value("mountIndex", -1) == 1);
        remove_device(router, "onstep", "telescope", 9622);
    }
#endif

#ifdef ALPACACORE_ENABLE_BISQUE
    {
        // bisque / telescope (TheSkyX TCP)
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "bisque"},
                                           {"deviceType", "telescope"},
                                           {"deviceNumber", 9617},
                                           {"host", "skyx.test"},
                                           {"tcpPort", 3041}},
                                          "Telescope", 9617);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("host", "") == "skyx.test");
        EXPECT(cfg.value("tcpPort", -1) == 3041);
        remove_device(router, "bisque", "telescope", 9617);
    }
#endif

    // =====================================================================
    // Issue open-astro/AlpacaBridge#647: every (vendor, deviceType) pair the
    // registration chain accepts is round-tripped from BOTH config sources.
    //   * API:       POST configuredevice -> ConfigSource::Api (roundtrip_config
    //                above; it never reaches the persisted path).
    //   * Persisted: the same config written to registered_devices.json and
    //                loaded by a new Router -> ConfigSource::Persisted.
    // Every field configureddevices shows must equal the expected object, and
    // no extra key may appear. Device numbers 97xx (assigned in order below).
    //
    // NOT round-tripped in this fake-only build (a pair listed here is not
    // covered from either source, and why):
    //   * astroasis / focuser by focuserIndex: the by-index constructor scans
    //     the USB bus eagerly (see the astroasis note in the #102 block), so
    //     only the hidPath form is round-tripped.
    //   * Every arm whose "auto", empty-connectionType or by-index path probes
    //     hardware or the network while the driver is constructed (the auto
    //     paths of the ioptron, synscan, skywatcher, onstep and celestron
    //     mounts, the ioptron network auto-scan, the by-index paths of the
    //     ioptron, gemini and qhy focusers, and any other auto/by-index arm
    //     that only registers with a device attached, e.g. the ioptron
    //     filterwheel, qhy cfw3, gemini focuser and gemini lite cover
    //     calibrator auto forms). Probing opens serial ports and scans the LAN,
    //     so it is not fake-only and is deliberately NOT exercised here; their
    //     serial and network forms ARE round-tripped, and the #508 items that
    //     ride on those probe paths (items 3 and 4, and item 1 for the mounts'
    //     by-index arms) are left unpinned until a seam exists to fake the probe.
    // =====================================================================
    {
        struct RoundtripCase {
            std::string label;
            std::string alpaca_type;
            nlohmann::json posted;
            nlohmann::json expected;
        };
        std::vector<RoundtripCase> cases;
        int next_number = 9700;
        // `posted` and `expected` hold everything except the three identity
        // keys, which sanitize_device_config always copies (vendor, deviceType,
        // deviceNumber) and which are added to both here.
        const auto add = [&](const std::string& vendor, const std::string& device_type, const std::string& alpaca_type,
                             const std::string& variant, const char* posted_json, const char* expected_json) {
            const nlohmann::json identity = {
                {"vendor", vendor}, {"deviceType", device_type}, {"deviceNumber", ++next_number}};
            RoundtripCase c;
            c.label = vendor + "/" + device_type + (variant.empty() ? "" : " " + variant);
            c.alpaca_type = alpaca_type;
            c.posted = nlohmann::json::parse(posted_json);
            c.expected = nlohmann::json::parse(expected_json);
            c.posted.update(identity);
            c.expected.update(identity);
            cases.push_back(std::move(c));
        };
        // Vendor-agnostic tail that sanitize_device_config copies for every vendor.
        const char* const kTail =
            R"("responseTimeoutMs":4000,"apertureDiameter":0.2,"focalLength":1.0,"siteLatitude":39.7392,)"
            R"("siteLongitude":-104.9903,"siteElevation":1609.0,"learnSiteFromClient":true,"syncTimeOnConnect":false)";
        const auto with_tail = [&](const std::string& base) {
            return std::string("{") + base + (base.empty() ? "" : ",") + kTail + "}";
        };

#ifdef ALPACACORE_ENABLE_ZWO
        add("zwo", "camera", "Camera", "", R"({"cameraIndex":1,"cameraId":7})", R"({"cameraIndex":1,"cameraId":7})");
        add("zwo", "filterwheel", "FilterWheel", "",
            R"({"filterwheelIndex":1,"filterwheelId":5,"filterNames":["L","R","G","B","Ha"]})",
            R"({"filterwheelIndex":1,"filterwheelId":5,"filterNames":["L","R","G","B","Ha"]})");
        add("zwo", "focuser", "Focuser", "", R"({"focuserIndex":1,"focuserId":3})",
            R"({"focuserIndex":1,"focuserId":3})");
        add("zwo", "rotator", "Rotator", "", R"({"rotatorIndex":1,"rotatorId":2})",
            R"({"rotatorIndex":1,"rotatorId":2})");
        add("zwo", "switch", "Switch", "dewheater", R"({"switchType":"dewheater","cameraIndex":1,"cameraId":4})",
            R"({"switchType":"dewheater","cameraIndex":1,"cameraId":4})");
        // ports/pwmFrequencyHz survive for the three ASIAIR variants; gpioChip
        // only for the libgpiod two, devicePath only for the RK3568.
        add("zwo", "switch", "Switch", "asiair",
            R"({"switchType":"asiair","gpioChip":"/dev/gpiochip0","devicePath":"/dev/x","pwmFrequencyHz":200,)"
            R"("ports":[{"gpio":12,"name":"Mount","pwm":false},{"gpio":13,"name":"Dew","pwm":true}]})",
            R"({"switchType":"asiair","gpioChip":"/dev/gpiochip0","pwmFrequencyHz":200,)"
            R"("ports":[{"gpio":12,"name":"Mount","pwm":false},{"gpio":13,"name":"Dew","pwm":true}]})");
        add("zwo", "switch", "Switch", "asiair-plus-picm4",
            R"({"switchType":"asiair-plus-picm4","gpioChip":"/dev/gpiochip0","devicePath":"/dev/x","pwmFrequencyHz":200,)"
            R"("ports":[{"gpio":12,"name":"Mount","pwm":false}]})",
            R"({"switchType":"asiair-plus-picm4","gpioChip":"/dev/gpiochip0","pwmFrequencyHz":200,)"
            R"("ports":[{"gpio":12,"name":"Mount","pwm":false}]})");
        add("zwo", "switch", "Switch", "asiair-plus-rk3568",
            R"({"switchType":"asiair-plus-rk3568","gpioChip":"/dev/gpiochip0","devicePath":"/dev/pwm-gpio-misc",)"
            R"("pwmFrequencyHz":50,"ports":[{"name":"DC1","pwm":true}]})",
            R"({"switchType":"asiair-plus-rk3568","devicePath":"/dev/pwm-gpio-misc","pwmFrequencyHz":50,)"
            R"("ports":[{"name":"DC1","pwm":true}]})");
        // zwo / telescope had no round-trip case at all before #647.
        add("zwo", "telescope", "Telescope", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB1","baudRate":9600,"host":"h","tcpPort":1,"cameraIndex":9})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB1","baudRate":9600,"cameraIndex":9})");
        add("zwo", "telescope", "Telescope", "network",
            R"({"connectionType":"network","host":"192.168.4.1","tcpPort":4030,"portPath":"/dev/x","baudRate":9600})",
            R"({"connectionType":"network","host":"192.168.4.1","tcpPort":4030})");
        add("zwo", "telescope", "Telescope", "auto", R"({"connectionType":"auto","portPath":"/dev/x"})",
            R"({"connectionType":"auto"})");
#endif

#ifdef ALPACACORE_ENABLE_QHY
        add("qhy", "camera", "Camera", "", R"({"cameraIndex":1,"cameraId":"QHY-TEST-1","bogusKey":1})",
            R"({"cameraIndex":1,"cameraId":"QHY-TEST-1"})");
        add("qhy", "focuser", "Focuser", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyACM3","focuserIndex":1,"maxStep":30000,"reverse":true,)"
            R"("speed":4,"holdForce":true,"holdIhold":6,"holdIrun":12,"temperatureSource":"chip","cameraIndex":7})",
            R"({"connectionType":"serial","portPath":"/dev/ttyACM3","focuserIndex":1,"maxStep":30000,"reverse":true,)"
            R"("speed":4,"holdForce":true,"holdIhold":6,"holdIrun":12,"temperatureSource":"chip"})");
        add("qhy", "filterwheel", "FilterWheel", "integrated",
            R"({"wheelType":"integrated","cameraIndex":3,"cameraId":"QHY-CFW-1","filterNames":["L","R"],)"
            R"("connectionType":"serial","portPath":"/dev/x","filterwheelIndex":4})",
            R"({"wheelType":"integrated","cameraIndex":3,"cameraId":"QHY-CFW-1","filterNames":["L","R"]})");
        add("qhy", "filterwheel", "FilterWheel", "cfw3-usb",
            R"({"wheelType":"cfw3-usb","connectionType":"serial","portPath":"/dev/ttyUSB7","filterwheelIndex":1,)"
            R"("filterNames":["L","R","G"],"cameraIndex":3,"cameraId":"x"})",
            R"({"wheelType":"cfw3-usb","connectionType":"serial","portPath":"/dev/ttyUSB7","filterwheelIndex":1,)"
            R"("filterNames":["L","R","G"]})");
#endif

#ifdef ALPACACORE_ENABLE_SVBONY
        add("svbony", "camera", "Camera", "", R"({"cameraIndex":2,"cameraId":"x"})", R"({"cameraIndex":2})");
#endif
#ifdef ALPACACORE_ENABLE_GPHOTO
        add("gphoto", "camera", "Camera", "", R"({"cameraIndex":1,"cameraId":"x"})", R"({"cameraIndex":1})");
#endif

#ifdef ALPACACORE_ENABLE_TOUPTEK
        // touptek camera, focuser and filterwheel had no roundtrip_config() case before #647.
        add("touptek", "camera", "Camera", "", R"({"cameraIndex":1,"focuserIndex":5,"focuserId":"z"})",
            R"({"cameraIndex":1,"focuserIndex":5,"focuserId":"z"})");
        add("touptek", "focuser", "Focuser", "", R"({"focuserIndex":2,"focuserId":"AAF-1","cameraIndex":3})",
            R"({"cameraIndex":3,"focuserIndex":2,"focuserId":"AAF-1"})");
        add("touptek", "filterwheel", "FilterWheel", "",
            R"({"filterwheelIndex":1,"filterwheelId":"AFW-1","filterNames":["L","R","G","B","Ha"],"cameraIndex":3})",
            R"({"filterwheelIndex":1,"filterwheelId":"AFW-1","filterNames":["L","R","G","B","Ha"]})");
        add("touptek", "switch", "Switch", "thermal", R"({"switchType":"thermal","cameraIndex":2,"gpioChip":"/dev/x"})",
            R"({"switchType":"thermal","cameraIndex":2})");
#ifdef ALPACACORE_TOUPTEK_STELLAVITA
        add("touptek", "switch", "Switch", "stellavita",
            R"({"switchType":"stellavita","gpioChip":"/dev/gpiochip0","pwmFrequencyHz":100,"cameraIndex":2,)"
            R"("ports":[{"name":"Flat Panel","pwm":true},{"name":"Camera","pwm":false}]})",
            R"({"switchType":"stellavita","gpioChip":"/dev/gpiochip0","pwmFrequencyHz":100,)"
            R"("ports":[{"name":"Flat Panel","pwm":true},{"name":"Camera","pwm":false}]})");
#endif
#endif

#ifdef ALPACACORE_ENABLE_PLAYERONE
        add("playerone", "camera", "Camera", "", R"({"cameraIndex":3,"filterwheelIndex":2})", R"({"cameraIndex":3})");
        add("playerone", "filterwheel", "FilterWheel", "",
            R"({"filterwheelIndex":1,"filterNames":["L","R","G"],"cameraIndex":3})",
            R"({"filterwheelIndex":1,"filterNames":["L","R","G"]})");
        // playerone switch (the thermal dew heater/fan) had no roundtrip_config() case before #647.
        add("playerone", "switch", "Switch", "", R"({"cameraIndex":2,"switchType":"x"})", R"({"cameraIndex":2})");
        // iOptron iCAM is a rebadged Player One camera, routed to that SDK.
        add("ioptron", "camera", "Camera", "", R"({"cameraIndex":2,"connectionType":"serial"})",
            R"({"cameraIndex":2})");
#endif

#ifdef ALPACACORE_ENABLE_IOPTRON
#ifdef ALPACACORE_IOPTRON_POWERBOX
        // ioptron switch (iMate PowerBox) had no roundtrip_config() case before #647.
        add("ioptron", "switch", "Switch", "",
            R"({"gpioChip":"/dev/gpiochip0","pwmFrequencyHz":100,"ports":[{"name":"P1","pwm":true}],"cameraIndex":1})",
            R"({"gpioChip":"/dev/gpiochip0","pwmFrequencyHz":100,"ports":[{"name":"P1","pwm":true}]})");
#endif
        add("ioptron", "telescope", "Telescope", "serial + tail",
            with_tail(
                R"("connectionType":"serial","portPath":"/dev/ttyUSB6","baudRate":115200,"mountIndex":1,"host":"h")")
                .c_str(),
            with_tail(R"("connectionType":"serial","portPath":"/dev/ttyUSB6","baudRate":115200,"mountIndex":1)")
                .c_str());
        add("ioptron", "telescope", "Telescope", "network",
            R"({"connectionType":"network","host":"192.168.1.9","tcpPort":4030,"mountIndex":2,"portPath":"/dev/x"})",
            R"({"connectionType":"network","host":"192.168.1.9","tcpPort":4030,"mountIndex":2})");
        add("ioptron", "focuser", "Focuser", "",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB7","focuserIndex":2,"model":"iafs2","baudRate":9})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB7","focuserIndex":2,"model":"iafs2"})");
        add("ioptron", "filterwheel", "FilterWheel", "",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB8","filterwheelIndex":1,"model":"iefw18",)"
            R"("filterNames":["L","R","G","B","Ha"],"baudRate":9})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB8","filterwheelIndex":1,"model":"iefw18",)"
            R"("filterNames":["L","R","G","B","Ha"]})");
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
        add("synscan", "telescope", "Telescope", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB5","baudRate":9600,"synscanVersion":"v4","mountIndex":2,"host":"h"})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB5","baudRate":9600,"synscanVersion":"v4","mountIndex":2})");
        add("synscan", "telescope", "Telescope", "network",
            R"({"connectionType":"network","host":"192.168.1.5","tcpPort":11880,"synscanVersion":"v3","portPath":"/dev/x"})",
            R"({"connectionType":"network","host":"192.168.1.5","tcpPort":11880,"synscanVersion":"v3"})");
#endif

#ifdef ALPACACORE_ENABLE_SKYWATCHER
        // Site coordinates are mandatory for this vendor from the API (#274).
        add("skywatcher", "telescope", "Telescope", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB6","baudRate":9600,"siteLatitude":39.7392,)"
            R"("siteLongitude":-104.9903,"siteElevation":1609.0,"mountIndex":1,"host":"h","udpPort":1})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB6","baudRate":9600,"siteLatitude":39.7392,)"
            R"("siteLongitude":-104.9903,"siteElevation":1609.0,"mountIndex":1})");
        add("skywatcher", "telescope", "Telescope", "network",
            R"({"connectionType":"network","host":"192.168.4.1","udpPort":11880,"siteLatitude":-33.87,)"
            R"("siteLongitude":151.21,"portPath":"/dev/x","tcpPort":1})",
            R"({"connectionType":"network","host":"192.168.4.1","udpPort":11880,"siteLatitude":-33.87,)"
            R"("siteLongitude":151.21})");
#endif

#ifdef ALPACACORE_ENABLE_ONSTEP
        add("onstep", "telescope", "Telescope", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyACM0","baudRate":9600,"mountIndex":1,"host":"h"})",
            R"({"connectionType":"serial","portPath":"/dev/ttyACM0","baudRate":9600,"mountIndex":1})");
#endif

#ifdef ALPACACORE_ENABLE_CELESTRON
        // celestron / telescope had no roundtrip_config() case before #647.
        add("celestron", "telescope", "Telescope", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB4","baudRate":9600,"mountIndex":1,"host":"h"})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB4","baudRate":9600,"mountIndex":1})");
        add("celestron", "telescope", "Telescope", "network",
            R"({"connectionType":"network","host":"192.168.1.7","tcpPort":2000,"mountIndex":2,"portPath":"/dev/x"})",
            R"({"connectionType":"network","host":"192.168.1.7","tcpPort":2000,"mountIndex":2})");
#endif

#ifdef ALPACACORE_ENABLE_BISQUE
        add("bisque", "telescope", "Telescope", "", R"({"host":"skyx.test","tcpPort":3041,"connectionType":"serial"})",
            R"({"host":"skyx.test","tcpPort":3041})");
#endif

#ifdef ALPACACORE_ENABLE_GEMINI
        add("gemini", "focuser", "Focuser", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB7","baudRate":19200,"focuserIndex":1,"panelIndex":2})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB7","baudRate":19200,"focuserIndex":1,"panelIndex":2})");
        add("gemini", "covercalibrator", "CoverCalibrator", "lite",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB8","baudRate":19200,"panelIndex":2})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB8","baudRate":19200,"panelIndex":2})");
        add("gemini", "covercalibrator", "CoverCalibrator", "v2",
            R"({"flatPanelModel":"v2","connectionType":"serial","portPath":"/dev/ttyUSB9","baudRate":19200,"panelIndex":3})",
            R"({"flatPanelModel":"v2","connectionType":"serial","portPath":"/dev/ttyUSB9","baudRate":19200,"panelIndex":3})");
        add("gemini", "covercalibrator", "CoverCalibrator", "pro",
            R"({"flatPanelModel":"pro","connectionType":"auto","panelIndex":1,"portPath":"/dev/x","baudRate":9})",
            R"({"flatPanelModel":"pro","connectionType":"auto","panelIndex":1})");
        add("gemini", "switch", "Switch", "pdh-adv3 auto",
            R"({"switchType":"pdh-adv3","connectionType":"auto","hubIndex":1,"focuserIndex":4})",
            R"({"switchType":"pdh-adv3","connectionType":"auto","hubIndex":1,"focuserIndex":4})");
        add("gemini", "switch", "Switch", "pdh-adv3 serial",
            R"({"switchType":"pdh-adv3","connectionType":"serial","portPath":"/dev/ttyUSB3","baudRate":19200,"hubIndex":1})",
            R"({"switchType":"pdh-adv3","connectionType":"serial","portPath":"/dev/ttyUSB3","baudRate":19200,"hubIndex":1})");
#endif

#ifdef ALPACACORE_ENABLE_ASTROASIS
        add("astroasis", "focuser", "Focuser", "hidPath", R"({"hidPath":"/dev/hidraw3","focuserIndex":2})",
            R"({"hidPath":"/dev/hidraw3","focuserIndex":2})");
#endif

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
        // wandererastro covercalibrator and filterwheel had no roundtrip_config() case before #647.
        add("wandererastro", "covercalibrator", "CoverCalibrator", "auto",
            R"({"connectionType":"auto","coverIndex":1,"portPath":"/dev/x"})",
            R"({"connectionType":"auto","coverIndex":1})");
        add("wandererastro", "covercalibrator", "CoverCalibrator", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB1","baudRate":19200,"coverIndex":1})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB1","baudRate":19200,"coverIndex":1})");
        add("wandererastro", "rotator", "Rotator", "auto", R"({"connectionType":"auto","rotatorIndex":1})",
            R"({"connectionType":"auto","rotatorIndex":1})");
        add("wandererastro", "rotator", "Rotator", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB2","baudRate":19200,"rotatorIndex":1})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB2","baudRate":19200,"rotatorIndex":1})");
        add("wandererastro", "filterwheel", "FilterWheel", "auto",
            R"({"connectionType":"auto","wandererFilterwheelIndex":1,"filterNames":["L","R","G","B","Ha","OIII","SII","Dark"],"filterwheelIndex":5})",
            R"({"connectionType":"auto","wandererFilterwheelIndex":1,"filterNames":["L","R","G","B","Ha","OIII","SII","Dark"]})");
        add("wandererastro", "filterwheel", "FilterWheel", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB3","baudRate":19200,"filterNames":["L","R","G","B","Ha","OIII","SII","Dark"]})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB3","baudRate":19200,"filterNames":["L","R","G","B","Ha","OIII","SII","Dark"]})");
        add("wandererastro", "switch", "Switch", "auto",
            R"({"switchType":"wandererbox-pro-v3","connectionType":"auto","boxIndex":1})",
            R"({"switchType":"wandererbox-pro-v3","connectionType":"auto","boxIndex":1})");
        add("wandererastro", "switch", "Switch", "serial",
            R"({"switchType":"wandererbox-pro-v3","connectionType":"serial","portPath":"/dev/ttyUSB4","baudRate":19200})",
            R"({"switchType":"wandererbox-pro-v3","connectionType":"serial","portPath":"/dev/ttyUSB4","baudRate":19200})");
#endif

#ifdef ALPACACORE_ENABLE_WEEWX
        add("weewx", "observingconditions", "ObservingConditions", "",
            R"({"weewxUrl":"http://weewx.test:8998/current.json","pollIntervalSeconds":300,"timeoutMs":2500,"cameraIndex":1})",
            R"({"weewxUrl":"http://weewx.test:8998/current.json","pollIntervalSeconds":300,"timeoutMs":2500})");
#endif

        for (const auto& c : cases) {
            const int number = c.posted.value("deviceNumber", -1);
            const std::string vendor = c.posted.value("vendor", "");
            const std::string device_type = c.posted.value("deviceType", "");

            const auto api = api_attempt(router, c.posted, c.alpaca_type);
            if (!api.ok || api.config != c.expected) {
                std::cerr << "#647 API round trip differs for " << c.label << "\n  error:    " << api.message
                          << "\n  expected: " << c.expected.dump() << "\n  actual:   " << api.config.dump() << "\n";
            }
            EXPECT(api.ok);
            EXPECT(api.config == c.expected);
            remove_device(router, vendor, device_type, number);

            const auto persisted = persisted_attempt(c.posted, c.alpaca_type);
            if (!persisted.listed || persisted.config != c.expected || !persisted.warnings.empty()) {
                std::cerr << "#647 persisted round trip differs for " << c.label << "\n  listed:   " << persisted.listed
                          << "\n  expected: " << c.expected.dump() << "\n  actual:   " << persisted.config.dump()
                          << "\n";
                for (const auto& w : persisted.warnings) {
                    std::cerr << "  WARN: " << w << "\n";
                }
            }
            EXPECT(persisted.listed);
            EXPECT(persisted.config == c.expected);
            // A valid saved config loads without a single WARN.
            EXPECT(persisted.warnings.empty());
        }
        // -----------------------------------------------------------------
        // Pinned behaviour: the two config sources DELIBERATELY differ on
        // invalid input (#380, generalising #353): the API rejects, a saved
        // config is registered anyway so it stays listed and editable in the
        // web UI. And the current, inconsistent behaviour behind
        // open-astro/AlpacaBridge#508, pinned AS IT IS TODAY. Each #508 case
        // below is commented with the item it pins and is expected to flip,
        // deliberately, when that item's fix lands.
        // -----------------------------------------------------------------
        struct Pin {
            std::string label;
            std::string alpaca_type;
            nlohmann::json posted;
            std::string api_error;  // empty: the API registers it
            nlohmann::json api_config;
            bool persisted_listed = false;
            nlohmann::json persisted_config;
            std::vector<std::string> warn;     // each must appear in some WARN of the load
            std::vector<std::string> no_warn;  // none may appear in any WARN of the load
        };
        std::vector<Pin> pins;
        const auto obj = [](std::initializer_list<std::string> parts) {
            std::string body;
            for (const auto& part : parts) {
                if (!part.empty()) {
                    body += (body.empty() ? "" : ",") + part;
                }
            }
            return "{" + body + "}";
        };
        const auto pin = [&](const std::string& label, const std::string& vendor, const std::string& device_type,
                             const std::string& alpaca_type, const std::string& posted_json,
                             const std::string& api_error, const std::string& api_config_json, bool persisted_listed,
                             const std::string& persisted_config_json, std::vector<std::string> warn,
                             std::vector<std::string> no_warn) {
            const nlohmann::json identity = {
                {"vendor", vendor}, {"deviceType", device_type}, {"deviceNumber", ++next_number}};
            Pin p;
            p.label = vendor + "/" + device_type + " " + label;
            p.alpaca_type = alpaca_type;
            p.posted = nlohmann::json::parse(posted_json);
            p.posted.update(identity);
            p.api_error = api_error;
            p.api_config = nlohmann::json::parse(api_config_json);
            p.api_config.update(identity);
            p.persisted_listed = persisted_listed;
            p.persisted_config = nlohmann::json::parse(persisted_config_json);
            p.persisted_config.update(identity);
            p.warn = std::move(warn);
            p.no_warn = std::move(no_warn);
            pins.push_back(std::move(p));
        };

        struct Mount {
            const char* vendor;
            const char* bad_type_message;  // the arm's own literal, they differ
            const char* site;              // mandatory from the API for this vendor (#274)
            bool empty_type_is_auto;       // zwo is the odd one out (#508 item 3)
        };
        std::vector<Mount> mounts;
        const char* const kSite = R"("siteLatitude":39.7392,"siteLongitude":-104.9903)";
        const char* const kAutoOrSerialOrNetwork = "Invalid connection type. Use 'auto', 'serial', or 'network'";
#ifdef ALPACACORE_ENABLE_IOPTRON
        mounts.push_back({"ioptron", kAutoOrSerialOrNetwork, "", true});
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
        mounts.push_back({"synscan", kAutoOrSerialOrNetwork, "", true});
#endif
#ifdef ALPACACORE_ENABLE_SKYWATCHER
        mounts.push_back({"skywatcher", kAutoOrSerialOrNetwork, kSite, true});
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
        mounts.push_back({"celestron", kAutoOrSerialOrNetwork, "", true});
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
        mounts.push_back({"onstep", "Invalid connection type. Use 'auto' or 'serial'", "", true});
#endif
#ifdef ALPACACORE_ENABLE_ZWO
        mounts.push_back({"zwo", "Invalid connection type. Use 'serial', 'network', or 'auto'", "", false});
#endif
        for (const auto& m : mounts) {
            const std::string site = m.site;
            const std::string vendor = m.vendor;
            const std::string warned_serial = "treating it as \"serial\"";

            // #380 / #353: an unrecognised connectionType. The API rejects with
            // the arm's own message. A saved one is normalised to "serial" IN
            // MEMORY (the WARN says so; connect then fails on the port path
            // instead of auto-probing), stays listed, and configureddevices
            // still shows the raw value: sanitize_device_config copies the file
            // value verbatim and only the registration sees the fallback.
            pin("connectionType \"carrier-pigeon\" (#380/#353)", vendor, "telescope", "Telescope",
                obj({R"("connectionType":"carrier-pigeon","portPath":"/dev/ttyUSB9")", site}), m.bad_type_message, "{}",
                true, obj({R"("connectionType":"carrier-pigeon")", site}),
                {"has connectionType \"carrier-pigeon\"", warned_serial}, {"Skipping persisted device"});

            // #508 item 2: connectionType is not case-folded. "Network" is
            // rejected by the API; a saved one is read as "serial" (WARN), the
            // host is not kept (sanitize tests == "network"), and the entry
            // keeps the raw "Network".
            pin("connectionType \"Network\" (#508 item 2)", vendor, "telescope", "Telescope",
                obj({R"("connectionType":"Network","host":"192.168.1.60")", site}), m.bad_type_message, "{}", true,
                obj({R"("connectionType":"Network")", site}), {"has connectionType \"Network\"", warned_serial},
                {"Skipping persisted device"});

            // #508 item 1 (the six mount arms that go through
            // reject_invalid_config): an empty portPath on serial is rejected
            // by the API and, from a saved config, WARNED about and registered
            // anyway. Contrast the arms further down that drop the entry.
            pin("serial with empty portPath (#508 item 1, mount arm)", vendor, "telescope", "Telescope",
                obj({R"("connectionType":"serial","portPath":"")", site}), "Serial port path is required", "{}", true,
                obj({R"("connectionType":"serial","portPath":"")", site}),
                {"will refuse to connect: Serial port path is required"}, {"Skipping persisted device"});

            // #508 item 3: an empty connectionType. zwo treats "" as
            // unrecognised: the API rejects it, a saved one is normalised to
            // "serial" (WARN) and stays listed. The five arms that treat ""
            // as "auto" are pinned by the probe cases below.
            if (!m.empty_type_is_auto) {
                pin("empty connectionType is NOT auto (#508 item 3)", vendor, "telescope", "Telescope",
                    obj({R"("connectionType":"")", site}), m.bad_type_message, "{}", true,
                    obj({R"("connectionType":"")", site}), {"has connectionType \"\"", warned_serial},
                    {"Skipping persisted device"});
            }
        }

#ifdef ALPACACORE_ENABLE_ONSTEP
        // #380: OnStep is serial-only, so a saved "network" is unrecognised for
        // it (the other network-capable mounts accept it). The entry keeps
        // "network" but not its host.
        pin("connectionType \"network\" (#380)", "onstep", "telescope", "Telescope",
            R"({"connectionType":"network","host":"192.168.1.60"})", "Invalid connection type. Use 'auto' or 'serial'",
            "{}", true, R"({"connectionType":"network"})",
            {"has connectionType \"network\"", "treating it as \"serial\""}, {"Skipping persisted device"});
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
        // The contrast that makes item 4 an inconsistency: SynScan rejects the
        // same config from the API and warns (but registers) from a saved one.
        pin("network with empty host (#508 item 4 contrast)", "synscan", "telescope", "Telescope",
            R"({"connectionType":"network","host":""})", "Host IP address is required", "{}", true,
            R"({"connectionType":"network","host":""})", {"will refuse to connect: Host IP address is required"},
            {"Skipping persisted device"});
#endif

#ifdef ALPACACORE_ENABLE_SKYWATCHER
        // #508 item 5: an out-of-range coordinate. The API rejects it; a saved
        // one is WARNED about and ignored by the driver (the #398 test above
        // reads the driver back), but the file entry keeps it: configureddevices
        // still shows 200.0, and the next save writes it back.
        pin("siteLatitude 200 (#508 item 5)", "skywatcher", "telescope", "Telescope",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB8","siteLatitude":200.0,"siteLongitude":172.6})",
            "siteLatitude 200.000000 is out of range: must be between -90.000000 and 90.000000 degrees", "{}", true,
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB8","siteLatitude":200.0,"siteLongitude":172.6})",
            {"siteLatitude 200.000000 is out of range", "The coordinate is ignored"}, {"Skipping persisted device"});
#endif

        // #508 item 1, the arms that DROP a saved entry on an empty portPath
        // instead of registering it with a WARN: it is rejected by the API and
        // is NOT registered after a restart. It is still listed, as a
        // "<vendor> (failed to load)" row (LoadError true, lower-case
        // DeviceType), which is how the web UI can show and edit it.
        const std::string kPortRequired = "portPath is required when connectionType is 'serial' (or use 'auto').";
        const auto drop_pin = [&](const std::string& vendor, const std::string& device_type,
                                  const std::string& alpaca_type, const std::string& message) {
            pin("serial with empty portPath is DROPPED (#508 item 1)", vendor, device_type, alpaca_type,
                R"({"connectionType":"serial","portPath":""})", message, "{}", false, "{}",
                {"Skipping persisted device: " + message}, {});
        };
#ifdef ALPACACORE_ENABLE_IOPTRON
        drop_pin("ioptron", "filterwheel", "FilterWheel", kPortRequired);
#endif
#ifdef ALPACACORE_ENABLE_QHY
        pin("cfw3-usb serial with empty portPath is DROPPED (#508 item 1)", "qhy", "filterwheel", "FilterWheel",
            R"({"wheelType":"cfw3-usb","connectionType":"serial","portPath":""})",
            "QHY CFW3 connectionType \"serial\" requires portPath", "{}", false, "{}",
            {"Skipping persisted device: QHY CFW3 connectionType \"serial\" requires portPath"}, {});
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
        drop_pin("gemini", "switch", "Switch", kPortRequired);
#endif
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
        drop_pin("wandererastro", "covercalibrator", "CoverCalibrator", kPortRequired);
        drop_pin("wandererastro", "rotator", "Rotator", kPortRequired);
        drop_pin("wandererastro", "filterwheel", "FilterWheel", kPortRequired);
        drop_pin("wandererastro", "switch", "Switch", kPortRequired);
#endif

        // #508 item 1, the arms that fall through to by-index auto-detect on an
        // empty portPath: registered from both sources, no WARN, and the entry
        // keeps connectionType "serial" with the empty portPath. (The other
        // three such arms probe hardware while constructing; see below.)
        const auto silent_pin = [&](const std::string& vendor, const std::string& device_type,
                                    const std::string& alpaca_type, const std::string& extra) {
            const std::string body = obj({R"("connectionType":"serial","portPath":"")", extra});
            pin("serial with empty portPath silently auto-detects (#508 item 1)", vendor, device_type, alpaca_type,
                body, "", body, true, body, {}, {"Serial port path is required", "Skipping persisted device"});
        };
#ifdef ALPACACORE_ENABLE_GEMINI
        silent_pin("gemini", "covercalibrator", "CoverCalibrator", "");
#endif

        for (const auto& p : pins) {
            const int number = p.posted.value("deviceNumber", -1);
            const std::string vendor = p.posted.value("vendor", "");
            const std::string device_type = p.posted.value("deviceType", "");

            const auto api = api_attempt(router, p.posted, p.alpaca_type);
            if (api.ok != p.api_error.empty() || (api.ok && api.config != p.api_config) ||
                (!api.ok && api.message != p.api_error)) {
                std::cerr << "#647 API pin differs for " << p.label << "\n  expected error: " << p.api_error
                          << "\n  actual ok/error: " << api.ok << " / " << api.message
                          << "\n  expected config: " << p.api_config.dump()
                          << "\n  actual config:   " << api.config.dump() << "\n";
            }
            EXPECT(api.ok == p.api_error.empty());
            if (api.ok) {
                EXPECT(api.config == p.api_config);
                remove_device(router, vendor, device_type, number);
            } else {
                EXPECT(api.message == p.api_error);
            }

            const auto persisted = persisted_attempt(p.posted, p.alpaca_type);
            bool warnings_ok = true;
            for (const auto& fragment : p.warn) {
                warnings_ok = warnings_ok && any_warning_contains(persisted.warnings, fragment);
            }
            for (const auto& fragment : p.no_warn) {
                warnings_ok = warnings_ok && !any_warning_contains(persisted.warnings, fragment);
            }
            if (persisted.listed != p.persisted_listed ||
                (persisted.listed && persisted.config != p.persisted_config) || !warnings_ok) {
                std::cerr << "#647 persisted pin differs for " << p.label
                          << "\n  expected listed: " << p.persisted_listed << " actual: " << persisted.listed
                          << "\n  expected config: " << p.persisted_config.dump()
                          << "\n  actual config:   " << persisted.config.dump() << "\n";
                for (const auto& w : persisted.warnings) {
                    std::cerr << "  WARN: " << w << "\n";
                }
            }
            EXPECT(persisted.listed == p.persisted_listed);
            if (!p.persisted_listed) {
                // Not registered is not the same as not listed: the failed
                // entry is shown with LoadError and its saved Config.
                EXPECT(persisted.failed_listed);
                EXPECT(persisted.failed_entry.value("LoadError", false));
                EXPECT(persisted.failed_entry.value("DeviceName", "") == vendor + " (failed to load)");
                EXPECT(persisted.failed_entry["Config"].value("vendor", "") == vendor);
            }
            if (persisted.listed) {
                EXPECT(persisted.config == p.persisted_config);
            }
            EXPECT(warnings_ok);
        }
    }

    // configureddevices surfaces Firmware and SdkVersion independently, each only
    // when the live driver reports that specific value.
    {
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        // Real device firmware only (e.g. WandererCover / a mount).
        auto firmware_dev = std::make_shared<FirmwareStubDriver>(9501, std::string("2025-05-04"));
        // Vendor SDK version only (e.g. ZWO camera — no device firmware API).
        auto sdk_dev = std::make_shared<FirmwareStubDriver>(9502, std::nullopt, std::string("1.7.7.0"));
        // Neither.
        auto silent = std::make_shared<FirmwareStubDriver>(9503, std::nullopt);
        EXPECT(registry.register_device(firmware_dev));
        EXPECT(registry.register_device(sdk_dev));
        EXPECT(registry.register_device(silent));

        const auto response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto json = nlohmann::json::parse(response.body());
        EXPECT(json.value("ErrorNumber", -1) == 0);

        bool checked_firmware = false;
        bool checked_sdk = false;
        bool checked_silent = false;
        for (const auto& entry : json["Value"]) {
            if (entry.value("DeviceType", "") != "CoverCalibrator") {
                continue;
            }
            if (entry.value("DeviceNumber", -1) == 9501) {
                EXPECT(entry.contains("Firmware"));
                EXPECT(entry.value("Firmware", "") == "2025-05-04");
                EXPECT(!entry.contains("SdkVersion"));
                checked_firmware = true;
            } else if (entry.value("DeviceNumber", -1) == 9502) {
                EXPECT(!entry.contains("Firmware"));
                EXPECT(entry.contains("SdkVersion"));
                EXPECT(entry.value("SdkVersion", "") == "1.7.7.0");
                checked_sdk = true;
            } else if (entry.value("DeviceNumber", -1) == 9503) {
                EXPECT(!entry.contains("Firmware"));
                EXPECT(!entry.contains("SdkVersion"));
                checked_silent = true;
            }
        }
        EXPECT(checked_firmware);
        EXPECT(checked_sdk);
        EXPECT(checked_silent);

        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9501);
        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9502);
        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9503);
    }

    // Issue #160: per-client Connected refcounting. Two clients sharing one
    // device (imaging app + guider on the same mount): the first client in
    // powers the upstream link, the last one out tears it down, and one
    // client's disconnect must never take the device away from the other.
    {
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto stub = std::make_shared<ConnectStubDriver>(9701);
        EXPECT(registry.register_device(stub));
        const std::string base = "/api/v1/covercalibrator/9701";

        // Before anyone connects: false for everyone.
        EXPECT(!get_connected_value(router, base, "1"));
        EXPECT(!get_connected_value(router, base, ""));

        // Client 1 connects: the device link comes up exactly once.
        put_connected(router, base, "1", true);
        EXPECT(stub->connect_count == 1);
        EXPECT(get_connected_value(router, base, "1"));

        // A client that never connected reads false even though the device is
        // up; a ClientID-less probe reads raw device state (legacy behavior).
        EXPECT(!get_connected_value(router, base, "2"));
        EXPECT(get_connected_value(router, base, ""));

        // Client 2 joins: no second upstream connect.
        put_connected(router, base, "2", true);
        EXPECT(stub->connect_count == 1);
        EXPECT(get_connected_value(router, base, "2"));

        // Client 2 leaves: the device MUST stay up for client 1 (the bug in
        // issue #160 tore it down here).
        put_connected(router, base, "2", false);
        EXPECT(stub->disconnect_count == 0);
        EXPECT(stub->get_connected());
        EXPECT(get_connected_value(router, base, "1"));
        EXPECT(!get_connected_value(router, base, "2"));

        // Last client out: now the link is torn down.
        put_connected(router, base, "1", false);
        EXPECT(stub->disconnect_count == 1);
        EXPECT(!stub->get_connected());

        // Disconnecting a client that was never registered on a live device
        // must not touch the link.
        put_connected(router, base, "1", true);
        EXPECT(stub->connect_count == 2);
        put_connected(router, base, "99", false);
        EXPECT(stub->disconnect_count == 1);
        EXPECT(stub->get_connected());

        // Upstream failure: the link dies underneath the bridge. Every
        // client's registration is invalidated so all observers see the
        // disconnect, and a reconnect works from a clean slate.
        stub->drop_link();
        EXPECT(!get_connected_value(router, base, "1"));
        put_connected(router, base, "1", true);
        EXPECT(stub->connect_count == 3);
        EXPECT(get_connected_value(router, base, "1"));
        put_connected(router, base, "1", false);
        EXPECT(stub->disconnect_count == 2);

        // Platform 7 connect/disconnect endpoints share the same refcount.
        route_request(router, "PUT", base + "/connect", "ClientID=1");
        route_request(router, "PUT", base + "/connect", "ClientID=2");
        EXPECT(stub->connect_count == 4);
        route_request(router, "PUT", base + "/disconnect", "ClientID=1");
        EXPECT(stub->get_connected());
        route_request(router, "PUT", base + "/disconnect", "ClientID=2");
        EXPECT(!stub->get_connected());

        // JSON PUT bodies carry ClientID too (numeric JSON ClientID).
        const auto resp = route_request(router, "PUT", base + "/connected", R"({"Connected": true, "ClientID": 7})");
        const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
        EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
        EXPECT(get_connected_value(router, base, "7"));
        EXPECT(!get_connected_value(router, base, "8"));
        put_connected(router, base, "7", false);
        EXPECT(!stub->get_connected());

        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9701);
    }

    // A false link-health getter does not prove driver teardown completed.
    // Both Platform 6 and 7 must deliver an explicit disconnect to the driver.
    {
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto stub = std::make_shared<ConnectStubDriver>(9790);
        EXPECT(registry.register_device(stub));
        const std::string base = "/api/v1/covercalibrator/9790";
        for (bool platform7 : {false, true}) {
            put_connected(router, base, "445", true);
            stub->drop_link();
            EXPECT(!get_connected_value(router, base, "445"));
            EXPECT(stub->cleanup_pending);
            if (platform7)
                route_request(router, "PUT", base + "/disconnect", "ClientID=445");
            else
                put_connected(router, base, "445", false);
            EXPECT(!stub->cleanup_pending);
            EXPECT(!stub->get_connected());
        }
        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9790);
    }

    // Issue #163: the client key is qualified by peer address, so two clients
    // that omit ClientID (or reuse the same one) on DIFFERENT hosts get
    // distinct registry slots and can no longer shadow-disconnect each other.
    {
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto stub = std::make_shared<ConnectStubDriver>(9702);
        EXPECT(registry.register_device(stub));
        const std::string base = "/api/v1/covercalibrator/9702";

        // Two anonymous (no-ClientID) clients on different hosts.
        put_connected(router, base, "", true, "10.0.0.1");
        put_connected(router, base, "", true, "10.0.0.2");
        EXPECT(stub->connect_count == 1);

        // Host 2's anonymous disconnect must not drop host 1's link (the
        // pre-#163 shared anonymous slot did exactly that).
        put_connected(router, base, "", false, "10.0.0.2");
        EXPECT(stub->disconnect_count == 0);
        EXPECT(stub->get_connected());

        // Last anonymous client out tears it down.
        put_connected(router, base, "", false, "10.0.0.1");
        EXPECT(stub->disconnect_count == 1);

        // Same ClientID from different hosts are distinct clients too, and
        // GET answers per (ClientID, host).
        put_connected(router, base, "5", true, "10.0.0.1");
        put_connected(router, base, "5", true, "10.0.0.2");
        EXPECT(stub->connect_count == 2);
        EXPECT(get_connected_value(router, base, "5", "10.0.0.1"));
        put_connected(router, base, "5", false, "10.0.0.2");
        EXPECT(stub->get_connected());
        EXPECT(get_connected_value(router, base, "5", "10.0.0.1"));
        EXPECT(!get_connected_value(router, base, "5", "10.0.0.2"));
        put_connected(router, base, "5", false, "10.0.0.1");
        EXPECT(!stub->get_connected());

        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9702);
    }

    // Security: path traversal via the static-file handler must be rejected
    // (404) without leaking file contents (audit finding C1).
    {
        const char* traversal_paths[] = {"/web/../../../../etc/passwd", "/web/../secret",
                                         "/web/../../AlpacaHTTP/CMakeLists.txt", "/web/subdir/../../secret"};
        for (const char* path : traversal_paths) {
            const auto resp = route_request(router, "GET", path);
            EXPECT(resp.status_code() == 404 || resp.status_code() == 403 || resp.status_code() == 400);
            EXPECT(resp.body().find("root:") == std::string::npos);
            EXPECT(resp.body().find("cmake_minimum_required") == std::string::npos);
        }
    }

    // Security: Content-Length must be bounded and validated (audit finding
    // M1). An absurd or malformed value must fail parsing rather than drive a
    // multi-gigabyte body_.resize().
    {
        alpacahttp::Request bad_request;

        // Hostile size (about 4 GB) — over the kMaxBodyBytes cap.
        std::string oversize =
            "POST /management/v1/configuredevice HTTP/1.1\r\n"
            "Content-Length: 4294967295\r\n\r\n{}";
        EXPECT(!bad_request.parse(oversize));

        // Just over the cap.
        std::string over_cap =
            "POST /management/v1/configuredevice HTTP/1.1\r\n"
            "Content-Length: " +
            std::to_string(alpacahttp::Request::kMaxBodyBytes + 1) + "\r\n\r\n{}";
        EXPECT(!bad_request.parse(over_cap));

        // Non-numeric and overflowing values must be rejected, not ignored.
        std::string non_numeric =
            "POST /management/v1/configuredevice HTTP/1.1\r\n"
            "Content-Length: banana\r\n\r\n{}";
        EXPECT(!bad_request.parse(non_numeric));

        std::string overflow =
            "POST /management/v1/configuredevice HTTP/1.1\r\n"
            "Content-Length: 99999999999999999999999999\r\n\r\n{}";
        EXPECT(!bad_request.parse(overflow));

        // A well-formed request within the cap still parses.
        alpacahttp::Request good_request;
        std::string good =
            "POST /management/v1/configuredevice HTTP/1.1\r\n"
            "Content-Length: 2\r\n\r\n{}";
        EXPECT(good_request.parse(good));
        EXPECT(good_request.body() == "{}");
    }

    // Host-clock wiring (issue #302). The decision logic inside HostClock is
    // covered by its own unit tests; what had no coverage was the router's
    // use of it, which is what carries the #289 feature. With the syscalls
    // faked through set_host_clock_hooks(), none of this touches the real
    // system clock, so CI can run it.
    {
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto scope = std::make_shared<TelescopeClockStubDriver>(9801);
        EXPECT(registry.register_device(scope));
        const std::string base = "/api/v1/telescope/9801";
        // 2001-01-01T00:00:00Z: inside the 2000-2100 window and an instant
        // the host clock can never be within 1 s of, so the step is never
        // classified as too small (a literal near "today" would fail once a
        // year, for two seconds).
        const std::string client_utc_body = R"({"UTCDate":"2001-01-01T00:00:00.000Z"})";
        const auto expected = std::chrono::system_clock::from_time_t(978307200);  // the same instant, as time_t

        // desc["Value"] is nlohmann's const operator[], which is a JSON_ASSERT
        // only -- under NDEBUG an error envelope (no "Value") dereferences
        // end() instead of failing cleanly. Check the envelope, then read.
        auto clock_field = [](const nlohmann::json& desc, const char* key) -> nlohmann::json {
            if (desc.is_discarded() || !desc.contains("Value") || !desc["Value"].contains(key)) {
                return nlohmann::json();
            }
            return desc["Value"][key];
        };

        // The sub-blocks below (six, the last carrying the five UTCDate
        // ladder cases) share this stub, so each starts from a known count
        // rather than inheriting the previous block's. Calling
        // this is what makes a block order-independent; a block that forgets
        // would assert against a carried-over number.
        auto fresh_counts = [&] {
            scope->utc_writes = 0;
            scope->on_utc_write = nullptr;
        };

        // An undisciplined host: the write must reach the setter exactly once,
        // carrying the client's value, and the driver must then be handed the
        // same instant.
        {
            // Locals first, router second: the hook lambdas capture these by
            // reference and the router owns the lambdas, so declaring the
            // router last means it is destroyed first and can never outlive
            // what it captured.
            int set_calls = 0;
            int set_calls_at_write = -1;  // set_calls as seen from inside the driver write
            std::chrono::system_clock::time_point set_to{};
            alpacahttp::Router clock_router;
            clock_router.set_host_clock_hooks([] { return false; },
                                              [&](std::chrono::system_clock::time_point tp, std::string&) {
                                                  ++set_calls;
                                                  set_to = tp;
                                                  return true;
                                              });
            fresh_counts();
            scope->on_utc_write = [&] { set_calls_at_write = set_calls; };
            const auto response = route_request(clock_router, "PUT", base + "/utcdate", client_utc_body);
            scope->on_utc_write = nullptr;
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
            EXPECT(set_calls == 1);
            EXPECT(set_to == expected);
            // Ordering: the clock is stepped first, then the driver is handed
            // the same value it was stepped to. The stub records how many
            // steps had happened when its write arrived; swapping the two
            // calls in the router makes this 0.
            EXPECT(set_calls_at_write == 1);
            EXPECT(scope->utc_writes == 1);
            EXPECT(scope->get_utc_date() == expected);

            // The step is now visible in the management readout.
            const auto desc = nlohmann::json::parse(
                route_request(clock_router, "GET", "/management/v1/description").body(), nullptr, false);
            EXPECT(clock_field(desc, "ClockSource") == "client");
        }

        // A disciplined host (NTP/chrony/GPS) is never stepped, however wrong
        // the client is -- but the driver still receives the value, because
        // UTCDate is the client's property to set.
        {
            int set_calls = 0;
            alpacahttp::Router clock_router;
            clock_router.set_host_clock_hooks([] { return true; },
                                              [&](std::chrono::system_clock::time_point, std::string&) {
                                                  ++set_calls;
                                                  return true;
                                              });
            fresh_counts();
            const auto response = route_request(clock_router, "PUT", base + "/utcdate", client_utc_body);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
            EXPECT(set_calls == 0);
            EXPECT(scope->utc_writes == 1);

            const auto desc = nlohmann::json::parse(
                route_request(clock_router, "GET", "/management/v1/description").body(), nullptr, false);
            EXPECT(clock_field(desc, "ClockSource") == "ntp");
            EXPECT(clock_field(desc, "ClockSynchronized") == true);
        }

        // The opt-out blocks the step without blocking the driver write. The
        // flag is set BEFORE the hooks are installed, which is what proves
        // syncSystemClockFromClients survives the seam. Since open-astro#399
        // it survives because the seam replaces only the clock's HOOKS and
        // the object holding the flag is never destroyed; before that it
        // survived because set_host_clock_hooks() saved and restored it by
        // hand around building a replacement clock. Either way, this order is
        // the thing that fails if the carry-over is lost (set_calls becomes 1).
        {
            int set_calls = 0;
            alpacahttp::Router clock_router;
            clock_router.set_sync_system_clock_from_clients(false);
            clock_router.set_host_clock_hooks([] { return false; },
                                              [&](std::chrono::system_clock::time_point, std::string&) {
                                                  ++set_calls;
                                                  return true;
                                              });
            fresh_counts();
            route_request(clock_router, "PUT", base + "/utcdate", client_utc_body);
            EXPECT(set_calls == 0);
            EXPECT(scope->utc_writes == 1);

            const auto desc = nlohmann::json::parse(
                route_request(clock_router, "GET", "/management/v1/description").body(), nullptr, false);
            EXPECT(clock_field(desc, "ClockSource") == "none");
            EXPECT(clock_field(desc, "SyncSystemClockFromClients") == false);
        }

        // open-astro#401: the UTCDate write has the same host-level effect as
        // the synctime endpoint (it can step the clock and latch ClockSource),
        // so it takes the same cross-origin guard. A foreign Origin is refused
        // with 403 before the clock or the driver is touched; a same-origin
        // write and one with no Origin (native clients) still go through.
        {
            int set_calls = 0;
            alpacahttp::Router clock_router;
            clock_router.set_host_clock_hooks([] { return false; },
                                              [&](std::chrono::system_clock::time_point, std::string&) {
                                                  ++set_calls;
                                                  return true;
                                              });
            fresh_counts();
            const auto send = [&](const std::string& method, const std::string& origin) {
                std::ostringstream raw;
                raw << method << " " << base << "/utcdate HTTP/1.1\r\n"
                    << "Host: localhost\r\n";
                if (!origin.empty()) {
                    raw << "Origin: " << origin << "\r\n";
                }
                raw << "Content-Type: text/plain\r\n"
                    << "Content-Length: " << client_utc_body.size() << "\r\n\r\n"
                    << client_utc_body;
                alpacahttp::Request request;
                EXPECT(request.parse(raw.str()));
                return clock_router.route(request, 1);
            };
            EXPECT(send("PUT", "http://evil.example").status_code() == 403);
            EXPECT(send("POST", "http://evil.example").status_code() == 403);
            EXPECT(set_calls == 0);
            EXPECT(scope->utc_writes == 0);

            EXPECT(send("PUT", "http://localhost").status_code() != 403);
            EXPECT(set_calls == 1);
            EXPECT(scope->utc_writes == 1);

            EXPECT(send("PUT", "").status_code() != 403);
            EXPECT(scope->utc_writes == 2);
        }

        // A host with no CAP_SYS_TIME: the refusal latches, so a later reader
        // can tell "nothing in this process will ever fix this clock" apart
        // from "a client has not written yet".
        {
            alpacahttp::Router clock_router;
            clock_router.set_host_clock_hooks([] { return false; },
                                              [](std::chrono::system_clock::time_point, std::string& error) {
                                                  error = "operation not permitted";
                                                  return false;
                                              });
            fresh_counts();
            const auto response = route_request(clock_router, "PUT", base + "/utcdate", client_utc_body);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            // A refused clock step is not a failed UTCDate write: the driver
            // still gets the value and the client still gets a success.
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
            EXPECT(scope->utc_writes == 1);

            const auto desc = nlohmann::json::parse(
                route_request(clock_router, "GET", "/management/v1/description").body(), nullptr, false);
            EXPECT(clock_field(desc, "ClockSource") == "none");
        }

        // The UTCDate outcome ladder (#397): the handler logs the step's
        // outcome at INFO for Stepped, WARN for Failed, WARN for
        // SkippedSynchronized past kClientDisagreementWarn, and DEBUG for
        // everything else. The blocks above assert the step and the driver
        // write; none asserted which branch was logged or at what level, and
        // a ladder that silently inverted (INFO where a WARN belongs) is what
        // #349 found on the connect warning before it asserted levels. The
        // sink is captured with the level, and the minimum log level is
        // lowered to Debug for the block so the DEBUG arm is observable at
        // all (the default Info floor drops it before the sink).
        {
            struct CapturedLine {
                alpacacore::logging::LogLevel level;
                std::string message;
            };
            std::vector<CapturedLine> captured;
            std::mutex captured_mutex;
            // Restored by a guard rather than straight-line statements, so the
            // restore does not depend on where the block exits (review note on
            // PR #475; EXPECT aborts today, but a non-aborting assert would
            // otherwise leak the lowered level into every later block).
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
            // The one line the handler writes for a UTCDate step, by its
            // fixed prefix; returns (level, message) so a case can pin both.
            // Exactly one outcome line per step: a second match returns
            // nullopt so the has_value() checks below fail loudly instead of
            // silently pinning whichever line came first.
            auto outcome_line = [&]() -> std::optional<CapturedLine> {
                std::lock_guard<std::mutex> lock(captured_mutex);
                std::optional<CapturedLine> found;
                for (const auto& line : captured) {
                    if (line.message.find("UTCDate from ") != std::string::npos &&
                        line.message.find(": host clock ") != std::string::npos) {
                        if (found) {
                            return std::nullopt;
                        }
                        found = line;
                    }
                }
                return found;
            };
            auto clear = [&] {
                std::lock_guard<std::mutex> lock(captured_mutex);
                captured.clear();
            };
            auto write_ok = [&](alpacahttp::Router& router_under_test) {
                const auto response = route_request(router_under_test, "PUT", base + "/utcdate", client_utc_body);
                const auto json = nlohmann::json::parse(response.body(), nullptr, false);
                EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
            };

            // Stepped: INFO, naming the outcome.
            {
                alpacahttp::Router clock_router;
                clock_router.set_host_clock_hooks(
                    [] { return false; }, [](std::chrono::system_clock::time_point, std::string&) { return true; });
                fresh_counts();
                clear();
                write_ok(clock_router);
                const auto line = outcome_line();
                EXPECT(line.has_value());
                EXPECT(line && line->level == alpacacore::logging::LogLevel::Info);
                EXPECT(line && line->message.find("host clock stepped") != std::string::npos);
            }

            // Failed (clock_settime refused): WARN, carrying the setter's
            // error text so the operator sees why.
            {
                alpacahttp::Router clock_router;
                clock_router.set_host_clock_hooks([] { return false; },
                                                  [](std::chrono::system_clock::time_point, std::string& error) {
                                                      error = "operation not permitted";
                                                      return false;
                                                  });
                fresh_counts();
                clear();
                write_ok(clock_router);
                const auto line = outcome_line();
                EXPECT(line.has_value());
                EXPECT(line && line->level == alpacacore::logging::LogLevel::Warn);
                EXPECT(line && line->message.find("operation not permitted") != std::string::npos);
            }

            // SkippedSynchronized with the client far out (the fixture's
            // 2001 instant against the real clock): WARN, naming the shared
            // threshold in ms.
            {
                alpacahttp::Router clock_router;
                clock_router.set_host_clock_hooks(
                    [] { return true; }, [](std::chrono::system_clock::time_point, std::string&) { return true; });
                fresh_counts();
                clear();
                write_ok(clock_router);
                const auto line = outcome_line();
                EXPECT(line.has_value());
                EXPECT(line && line->level == alpacacore::logging::LogLevel::Warn);
                EXPECT(line &&
                       line->message.find("NTP-disciplined host and client disagree by more than " +
                                          std::to_string(alpacacore::util::HostClock::kClientDisagreementWarn.count()) +
                                          " ms") != std::string::npos);
            }

            // SkippedSynchronized with the client in agreement: the quiet
            // arm, DEBUG. A stub that reports the host's own "now" as the
            // client value keeps the delta under the threshold.
            {
                alpacahttp::Router clock_router;
                clock_router.set_host_clock_hooks(
                    [] { return true; }, [](std::chrono::system_clock::time_point, std::string&) { return true; });
                fresh_counts();
                clear();
                const auto now = std::chrono::system_clock::now();
                const std::time_t now_t = std::chrono::system_clock::to_time_t(now);
                // Carry the milliseconds: to_time_t truncates to the second,
                // which alone ate up to ~1 s of the 2 s agreement margin
                // before any request time was added.
                const auto now_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
                char stamp[32];
                std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", std::gmtime(&now_t));
                char stamp_ms[8];
                std::snprintf(stamp_ms, sizeof(stamp_ms), ".%03lldZ", static_cast<long long>(now_ms));
                const std::string agreeing_body = std::string(R"({"UTCDate":")") + stamp + stamp_ms + R"("})";
                const auto response = route_request(clock_router, "PUT", base + "/utcdate", agreeing_body);
                const auto json = nlohmann::json::parse(response.body(), nullptr, false);
                EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
                const auto line = outcome_line();
                EXPECT(line.has_value());
                EXPECT(line && line->level == alpacacore::logging::LogLevel::Debug);
                EXPECT(line && line->message.find("NTP-disciplined host and client disagree") == std::string::npos);
            }

            // SkippedDisabled (the opt-out): DEBUG, naming the outcome.
            {
                alpacahttp::Router clock_router;
                clock_router.set_sync_system_clock_from_clients(false);
                clock_router.set_host_clock_hooks(
                    [] { return false; }, [](std::chrono::system_clock::time_point, std::string&) { return true; });
                fresh_counts();
                clear();
                write_ok(clock_router);
                const auto line = outcome_line();
                EXPECT(line.has_value());
                EXPECT(line && line->level == alpacacore::logging::LogLevel::Debug);
                EXPECT(line && line->message.find("syncSystemClockFromClients is off") != std::string::npos);
            }
        }

        // A hardware RTC the kernel booted from is reported as the source
        // while the clock is undisciplined and unstepped (#292).
        {
            alpacahttp::Router clock_router;
            clock_router.set_host_clock_hooks([] { return false; },
                                              [](std::chrono::system_clock::time_point, std::string&) { return true; },
                                              [] { return true; });
            const auto desc = nlohmann::json::parse(
                route_request(clock_router, "GET", "/management/v1/description").body(), nullptr, false);
            EXPECT(clock_field(desc, "ClockSource") == "rtc");
            EXPECT(clock_field(desc, "ClockSynchronized") == false);
        }

        // Both connect paths warn when, and only when, the clock is
        // undisciplined and unstepped. The warning is log-only, so the test
        // captures the log sink; a driver about to compute LST from a wrong
        // clock is the whole reason #289 exists, and nothing else would catch
        // the line being dropped from one of the two paths.
        {
            // The level is captured alongside the message: warn_if_clock_undisciplined()
            // ends in an INFO/WARN ladder (router.cpp: an RTC-booted host that a client can
            // still correct is INFO, everything else WARN), and a test that only matched the
            // text would pass with the ladder inverted.
            struct CapturedLine {
                alpacacore::logging::LogLevel level;
                std::string message;
            };
            std::vector<CapturedLine> captured;
            std::mutex captured_mutex;
            auto previous_sink = alpacacore::logging::get_log_sink();
            alpacacore::logging::set_log_sink(
                [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                    std::lock_guard<std::mutex> lock(captured_mutex);
                    captured.push_back({level, std::string(message)});
                });

            auto clock_warning_level = [&]() -> std::optional<alpacacore::logging::LogLevel> {
                std::lock_guard<std::mutex> lock(captured_mutex);
                for (const auto& line : captured) {
                    if (line.message.find("undisciplined host clock") != std::string::npos) {
                        return line.level;
                    }
                }
                return std::nullopt;
            };
            auto warned_about_clock = [&] { return clock_warning_level().has_value(); };
            // The RTC arm carries a different sentence ("connecting on the
            // hardware RTC's time"), so it needs its own matcher.
            auto rtc_line_level = [&]() -> std::optional<alpacacore::logging::LogLevel> {
                std::lock_guard<std::mutex> lock(captured_mutex);
                for (const auto& line : captured) {
                    if (line.message.find("hardware RTC's time") != std::string::npos) {
                        return line.level;
                    }
                }
                return std::nullopt;
            };
            auto clear = [&] {
                std::lock_guard<std::mutex> lock(captured_mutex);
                captured.clear();
            };

            // Route and assert the request itself succeeded. Without this the
            // two negative cases below (a disciplined host, an already-stepped
            // one) would pass vacuously if the PUT failed before reaching
            // warn_if_clock_undisciplined() -- a future guard throwing earlier
            // in the handler would look exactly like "no warning was logged".
            auto connect_ok = [&](alpacahttp::Router& router_under_test, const std::string& path,
                                  const std::string& body) {
                const auto response = route_request(router_under_test, "PUT", path, body);
                const auto json = nlohmann::json::parse(response.body(), nullptr, false);
                EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
            };

            // Legacy PUT connected, undisciplined host: warns.
            {
                auto scope_a = std::make_shared<TelescopeClockStubDriver>(9802);
                EXPECT(registry.register_device(scope_a));
                alpacahttp::Router clock_router;
                clock_router.set_host_clock_hooks(
                    [] { return false; }, [](std::chrono::system_clock::time_point, std::string&) { return true; });
                clear();
                connect_ok(clock_router, "/api/v1/telescope/9802/connected", "Connected=true");
                EXPECT(warned_about_clock());
                // No RTC on this host, so the ladder's else arm: WARN, not INFO.
                EXPECT(clock_warning_level() == alpacacore::logging::LogLevel::Warn);
                registry.unregister_device(alpacacore::DeviceType::Telescope, 9802);
            }

            // ITelescopeV4 PUT connect initiator, same host: also warns. NINA
            // 3.x prefers this path, so a warning on only one is no warning.
            {
                auto scope_b = std::make_shared<TelescopeClockStubDriver>(9803);
                EXPECT(registry.register_device(scope_b));
                alpacahttp::Router clock_router;
                clock_router.set_host_clock_hooks(
                    [] { return false; }, [](std::chrono::system_clock::time_point, std::string&) { return true; });
                clear();
                connect_ok(clock_router, "/api/v1/telescope/9803/connect", "");
                EXPECT(warned_about_clock());
                EXPECT(clock_warning_level() == alpacacore::logging::LogLevel::Warn);
                registry.unregister_device(alpacacore::DeviceType::Telescope, 9803);
            }

            // An NTP-disciplined host is quiet on both paths.
            {
                auto scope_c = std::make_shared<TelescopeClockStubDriver>(9804);
                EXPECT(registry.register_device(scope_c));
                alpacahttp::Router clock_router;
                clock_router.set_host_clock_hooks(
                    [] { return true; }, [](std::chrono::system_clock::time_point, std::string&) { return true; });
                clear();
                connect_ok(clock_router, "/api/v1/telescope/9804/connected", "Connected=true");
                EXPECT(!warned_about_clock());
                registry.unregister_device(alpacacore::DeviceType::Telescope, 9804);
            }

            // A clock a client has already stepped is quiet too: the host is
            // still STA_UNSYNC, but it now carries the client's time.
            {
                auto scope_d = std::make_shared<TelescopeClockStubDriver>(9805);
                EXPECT(registry.register_device(scope_d));
                alpacahttp::Router clock_router;
                clock_router.set_host_clock_hooks(
                    [] { return false; }, [](std::chrono::system_clock::time_point, std::string&) { return true; });
                route_request(clock_router, "PUT", "/api/v1/telescope/9805/utcdate", client_utc_body);
                clear();
                connect_ok(clock_router, "/api/v1/telescope/9805/connected", "Connected=true");
                EXPECT(!warned_about_clock());
                registry.unregister_device(alpacacore::DeviceType::Telescope, 9805);
            }

            // An RTC-booted host a client can still correct is the ladder's
            // INFO arm: the clock is undisciplined, but it came from hardware
            // and something will fix it, so the line is informational rather
            // than a warning. Without this the Warn assertions above cannot
            // tell the ladder from a constant.
            {
                auto scope_e = std::make_shared<TelescopeClockStubDriver>(9806);
                EXPECT(registry.register_device(scope_e));
                alpacahttp::Router clock_router;
                clock_router.set_host_clock_hooks(
                    [] { return false; }, [](std::chrono::system_clock::time_point, std::string&) { return true; },
                    [] { return true; });
                clear();
                connect_ok(clock_router, "/api/v1/telescope/9806/connected", "Connected=true");
                // Same phrase the WARN cases match, so the two arms are
                // distinguished by level alone.
                EXPECT(rtc_line_level() == alpacacore::logging::LogLevel::Info);
                registry.unregister_device(alpacacore::DeviceType::Telescope, 9806);
            }

            // The other two message variants, both on an RTC-booted host so
            // that RTC alone cannot be what picks the level (#397).
            //
            // Refused: a step was attempted and clock_settime said no (no
            // CAP_SYS_TIME). The refusal latches, so an RTC host that would
            // otherwise be the INFO arm is now WARN, and the message tells
            // the operator to set the clock outside the service rather than
            // recommending the Sync Time button that the same refusal killed.
            {
                auto scope_f = std::make_shared<TelescopeClockStubDriver>(9807);
                EXPECT(registry.register_device(scope_f));
                alpacahttp::Router clock_router;
                clock_router.set_host_clock_hooks([] { return false; },
                                                  [](std::chrono::system_clock::time_point, std::string& error) {
                                                      error = "operation not permitted";
                                                      return false;
                                                  },
                                                  [] { return true; });
                route_request(clock_router, "PUT", "/api/v1/telescope/9807/utcdate", client_utc_body);
                clear();
                connect_ok(clock_router, "/api/v1/telescope/9807/connected", "Connected=true");
                EXPECT(rtc_line_level() == alpacacore::logging::LogLevel::Warn);
                {
                    std::lock_guard<std::mutex> lock(captured_mutex);
                    bool refused_text = false;
                    bool sync_time_recommended = false;
                    bool sync_script_recommended = false;
                    for (const auto& line : captured) {
                        if (line.message.find("hardware RTC's time") == std::string::npos) {
                            continue;
                        }
                        refused_text |= line.message.find("setting the clock was refused") != std::string::npos;
                        sync_time_recommended |= line.message.find("Use the web UI's Sync Time") != std::string::npos;
                        sync_script_recommended |= line.message.find("scripts/sync-clock.sh") != std::string::npos;
                    }
                    EXPECT(refused_text);
                    EXPECT(!sync_time_recommended);
                    EXPECT(sync_script_recommended);  // the replacement advice, not just the absence of the old
                }
                registry.unregister_device(alpacacore::DeviceType::Telescope, 9807);
            }

            // Opt-out: syncSystemClockFromClients is off, so no client will
            // correct the clock. WARN even on an RTC host, and the message
            // points at Sync Time, which still works here because nothing
            // has been refused.
            {
                auto scope_g = std::make_shared<TelescopeClockStubDriver>(9808);
                EXPECT(registry.register_device(scope_g));
                alpacahttp::Router clock_router;
                clock_router.set_sync_system_clock_from_clients(false);
                clock_router.set_host_clock_hooks(
                    [] { return false; }, [](std::chrono::system_clock::time_point, std::string&) { return true; },
                    [] { return true; });
                clear();
                connect_ok(clock_router, "/api/v1/telescope/9808/connected", "Connected=true");
                EXPECT(rtc_line_level() == alpacacore::logging::LogLevel::Warn);
                {
                    std::lock_guard<std::mutex> lock(captured_mutex);
                    bool opt_out_text = false;
                    for (const auto& line : captured) {
                        if (line.message.find("hardware RTC's time") != std::string::npos) {
                            opt_out_text |=
                                line.message.find("syncSystemClockFromClients is off") != std::string::npos &&
                                line.message.find("Use the web UI's Sync Time") != std::string::npos;
                        }
                    }
                    EXPECT(opt_out_text);
                }
                registry.unregister_device(alpacacore::DeviceType::Telescope, 9808);
            }

            alpacacore::logging::set_log_sink(previous_sink);
        }

        // The RTC probe runs at startup and on the server's timer, never on a
        // request path (issue #314). On a bus-attached RTC the probe is an
        // I2C transaction that can block for the adapter timeout, and the two
        // readers are the ITelescopeV4 connect initiator -- timed against the
        // 1 s STANDARD target, with the connection op mutex held -- and the
        // description endpoint the web UI polls.
        {
            auto probe_calls = std::make_shared<int>(0);
            alpacahttp::Router clock_router;
            clock_router.set_host_clock_hooks([] { return false; },
                                              [](std::chrono::system_clock::time_point, std::string&) { return true; },
                                              [probe_calls] {
                                                  ++*probe_calls;
                                                  return true;
                                              });
            // Priming happened once, at install. Since open-astro#399 that is
            // the refresh_rtc() at the end of HostClock::set_hooks(): the
            // Router's own clock was constructed before these hooks existed
            // and ran the REAL host_booted_from_rtc(), which this counter
            // never sees.
            EXPECT(*probe_calls == 1);

            auto scope_e = std::make_shared<TelescopeClockStubDriver>(9806);
            EXPECT(registry.register_device(scope_e));

            // Every path that reads the answer, hammered: the description
            // endpoint the web UI polls, both connect paths, and a UTCDate
            // write. None of them may probe.
            for (int i = 0; i < 5; ++i) {
                route_request(clock_router, "GET", "/management/v1/description");
            }
            route_request(clock_router, "PUT", "/api/v1/telescope/9806/connected", "Connected=true");
            route_request(clock_router, "PUT", "/api/v1/telescope/9806/connected", "Connected=false");
            route_request(clock_router, "PUT", "/api/v1/telescope/9806/connect", "");
            route_request(clock_router, "PUT", "/api/v1/telescope/9806/utcdate", client_utc_body);
            EXPECT(*probe_calls == 1);

            // The readout still works after the clock was stepped. This asserts
            // the "client" branch of source(), which returns before consulting
            // has_rtc() at all -- the cached RTC answer is covered by the rtc
            // case below, not by this line.
            const auto desc = nlohmann::json::parse(
                route_request(clock_router, "GET", "/management/v1/description").body(), nullptr, false);
            EXPECT(!desc.is_discarded() && desc["Value"]["ClockSource"] == "client");
            EXPECT(*probe_calls == 1);

            // The off-request-path refresh the server's RTC probe thread calls is
            // the only thing that re-probes.
            clock_router.refresh_rtc_probe();
            EXPECT(*probe_calls == 2);

            registry.unregister_device(alpacacore::DeviceType::Telescope, 9806);
        }

        registry.unregister_device(alpacacore::DeviceType::Telescope, 9801);
    }

    // synctime management endpoint: GET reads the clock, POST validates the
    // epoch range before touching it. The actual clock_settime() succeeds only
    // with CAP_SYS_TIME, so the happy-path set is validated on hardware; here
    // we pin the routing, the read path, and every rejection path.
    {
        alpacahttp::Router router;

        const auto get_response = route_request(router, "GET", "/management/v1/synctime");
        const auto get_json = nlohmann::json::parse(get_response.body(), nullptr, false);
        EXPECT(!get_json.is_discarded() && get_json.value("ErrorNumber", -1) == 0);
        // Value must be a plausible current epoch (build machines are NTP-synced).
        EXPECT(get_json["Value"].is_number_integer());
        EXPECT(get_json["Value"].get<std::int64_t>() > 1600000000);  // after 2020-09

        // Out-of-range epochs are rejected without setting the clock. The
        // status check is not redundant with ErrorNumber: a 403 from the
        // cross-origin guard (issue #298) also carries a non-zero
        // ErrorNumber, so without it this loop would keep passing if the
        // guard ever started rejecting a request that carries no Origin at
        // all -- which is every non-browser client, Ara included. That is the
        // invariant most worth not breaking here.
        for (const auto* body : {"{\"Epoch\": 100}", "{\"Epoch\": 5000000000}", "{\"Epoch\": -1}", "{}", "not json"}) {
            const auto response = route_request(router, "POST", "/management/v1/synctime", body);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
            EXPECT(response.status_code() != 403);
        }

        // DELETE is not a supported method.
        const auto del_response = route_request(router, "DELETE", "/management/v1/synctime");
        const auto del_json = nlohmann::json::parse(del_response.body(), nullptr, false);
        EXPECT(!del_json.is_discarded() && del_json.value("ErrorNumber", 0) != 0);
        // ...and without an Origin it is refused as an unsupported method, not
        // by the guard, so the next case can attribute its 403 to the guard.
        EXPECT(del_response.status_code() != 403);

        // The guard runs before the method check, so a cross-origin DELETE is
        // refused for being cross-origin rather than for being a DELETE. The
        // CHANGELOG calls this out as a status change from 405 to 403; this
        // pins it.
        {
            std::ostringstream raw;
            raw << "DELETE /management/v1/synctime HTTP/1.1\r\n"
                << "Host: localhost\r\n"
                << "Origin: http://evil.example\r\n\r\n";
            alpacahttp::Request request;
            EXPECT(request.parse(raw.str()));
            const auto response = router.route(request, 1);
            EXPECT(response.status_code() == 403);
        }

        // CSRF guard (issue #298): this endpoint sets the system clock and,
        // since #291, marks the host client-stepped, so it takes the same
        // Origin check the wifi endpoints use. A cross-origin mutating
        // request is rejected with 403 before the body is even parsed.
        {
            const std::string body = "{\"Epoch\": 100}";
            std::ostringstream raw;
            raw << "POST /management/v1/synctime HTTP/1.1\r\n"
                << "Host: localhost\r\n"
                << "Origin: http://evil.example\r\n"
                << "Content-Type: text/plain\r\n"
                << "Content-Length: " << body.size() << "\r\n\r\n"
                << body;
            alpacahttp::Request request;
            EXPECT(request.parse(raw.str()));
            const auto response = router.route(request, 1);
            EXPECT(response.status_code() == 403);
        }

        // A same-origin request passes the guard. It still fails here --
        // the epoch is deliberately outside the handler's 2000-2100 window,
        // so the request is refused after the guard and clock_settime is
        // never reached. Both cases above use an out-of-range epoch for that
        // reason: a run with CAP_SYS_TIME (sudo, a root container, a root
        // shell on a test SBC) would otherwise set the machine's clock from
        // a unit test, which AlpacaCore/tests/test_host_clock.cpp forbids.
        // The distinction the case needs is still visible: 403 means the
        // guard fired, 200 with a non-zero ErrorNumber means it did not.
        {
            const std::string body = "{\"Epoch\": 100}";
            std::ostringstream raw;
            raw << "POST /management/v1/synctime HTTP/1.1\r\n"
                << "Host: localhost\r\n"
                << "Origin: http://localhost\r\n"
                << "Content-Type: application/json\r\n"
                << "Content-Length: " << body.size() << "\r\n\r\n"
                << body;
            alpacahttp::Request request;
            EXPECT(request.parse(raw.str()));
            const auto response = router.route(request, 1);
            // The concrete pass path, not just "not 403": a 404 from broken
            // routing would satisfy the negation too. 200 with the epoch
            // window's own complaint means the guard let it through and the
            // handler refused it on the epoch, which is what this case is
            // for.
            EXPECT(response.status_code() == 200);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
            EXPECT(json.value("ErrorMessage", "").find("Unix timestamp") != std::string::npos);
        }

        // A GET carrying a cross-origin Origin header changes nothing, so it
        // is still served rather than rejected.
        {
            std::ostringstream raw;
            raw << "GET /management/v1/synctime HTTP/1.1\r\n"
                << "Host: localhost\r\n"
                << "Origin: http://evil.example\r\n\r\n";
            alpacahttp::Request request;
            EXPECT(request.parse(raw.str()));
            const auto response = router.route(request, 1);
            EXPECT(response.status_code() == 200);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
            // A plausible epoch, so a served-but-empty reply cannot pass.
            EXPECT(json["Value"].is_number_integer() && json["Value"].get<std::int64_t>() > 1600000000);
        }
    }

    // wifi management endpoints: routing + input validation. The happy paths
    // need a running NetworkManager (validated on hardware); here we pin that
    // the routes resolve, replies are well-formed Alpaca JSON, and invalid
    // input is rejected regardless of whether NM is present on the build box.
    {
        alpacahttp::Router router;

        // status always answers with valid Alpaca JSON (ErrorNumber 0 with a
        // Value on NM boxes, or a WiFi error where the system bus/NM is absent).
        const auto status_response = route_request(router, "GET", "/management/v1/wifi/status");
        const auto status_json = nlohmann::json::parse(status_response.body(), nullptr, false);
        EXPECT(!status_json.is_discarded() && status_json.contains("ErrorNumber"));

        // Unknown sub-endpoint and wrong methods are rejected.
        for (const auto& [method, path] : {
                 std::pair{"GET", "/management/v1/wifi/bogus"},
                 std::pair{"PUT", "/management/v1/wifi/status"},
                 std::pair{"GET", "/management/v1/wifi/connect"},
                 std::pair{"DELETE", "/management/v1/wifi/profiles"},
             }) {
            const auto response = route_request(router, method, path);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        }

        // CSRF guard: a cross-origin mutating request (browser-attached
        // Origin header not matching Host) is rejected with 403; a
        // same-origin one passes the guard (and proceeds to validation).
        {
            alpacahttp::Request request;
            std::string body = "{\"Alpha2\": \"US\"}";
            std::ostringstream raw;
            raw << "PUT /management/v1/wifi/country HTTP/1.1\r\n"
                << "Host: localhost\r\n"
                << "Origin: http://evil.example\r\n"
                << "Content-Type: application/json\r\n"
                << "Content-Length: " << body.size() << "\r\n\r\n"
                << body;
            EXPECT(request.parse(raw.str()));
            const auto response = router.route(request, 1);
            EXPECT(response.status_code() == 403);

            alpacahttp::Request same_origin;
            std::ostringstream raw2;
            raw2 << "PUT /management/v1/wifi/country HTTP/1.1\r\n"
                 << "Host: localhost\r\n"
                 << "Origin: http://localhost\r\n"
                 << "Content-Type: application/json\r\n"
                 << "Content-Length: 2\r\n\r\n{}";
            EXPECT(same_origin.parse(raw2.str()));
            const auto ok_response = router.route(same_origin, 1);
            // Passes the guard; fails body validation (Alpha2 missing), not 403.
            EXPECT(ok_response.status_code() != 403);
        }

        // Body validation fires before any NM traffic.
        for (const auto& [path, body] : {
                 std::pair{"/management/v1/wifi/country", "{\"Alpha2\": \"usa\"}"},
                 std::pair{"/management/v1/wifi/country", "{}"},
                 std::pair{"/management/v1/wifi/profiles", "{\"Passphrase\": \"x\"}"},
                 std::pair{"/management/v1/wifi/connect", "not json"},
                 std::pair{"/management/v1/wifi/ap", "{\"Ssid\": \"x\", \"Band\": \"g\"}"},
                 std::pair{"/management/v1/wifi/radio", "{\"Enabled\": \"yes\"}"},
             }) {
            const auto response = route_request(router, "PUT", path, body);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        }
    }

    // Response header names are case-insensitive (RFC 7230 §3.2). The server's
    // keep-alive override does get_header("Connection") then set_header(
    // "Connection", ...) on top of whatever a handler set; with a
    // case-sensitive map a handler's "connection: keep-alive" would survive
    // beside the server's "Connection: close" and BOTH would go on the wire,
    // contradicting each other on a socket the server is about to close.
    {
        alpacahttp::Response response;
        response.set_header("connection", "keep-alive");
        EXPECT(response.get_header("Connection") == "keep-alive");
        response.set_header("Connection", "close");
        EXPECT(response.get_header("connection") == "close");
        const std::string wire = response.to_string();
        std::size_t occurrences = 0;
        std::string lower = wire;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (std::size_t pos = lower.find("\r\nconnection:"); pos != std::string::npos;
             pos = lower.find("\r\nconnection:", pos + 1)) {
            ++occurrences;
        }
        EXPECT(occurrences == 1);
        EXPECT(wire.find("Connection: close\r\n") != std::string::npos);
        // And the default still applies when nothing set it, under any casing.
        alpacahttp::Response bare;
        EXPECT(bare.to_string().find("Connection: close\r\n") != std::string::npos);
    }

    // Every response carries a Content-Length, so none is framed by
    // connection close (unframeable on a persistent connection). A response
    // with no body gets "Content-Length: 0"; set_body() already sets the
    // header, and to_string() must not emit a second one beside it.
    {
        alpacahttp::Response bare;
        EXPECT(bare.to_string().find("Content-Length: 0\r\n") != std::string::npos);

        alpacahttp::Response with_body;
        with_body.set_body("{}");
        const std::string wire = with_body.to_string();
        std::string lower = wire;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::size_t occurrences = 0;
        for (std::size_t pos = lower.find("\r\ncontent-length:"); pos != std::string::npos;
             pos = lower.find("\r\ncontent-length:", pos + 1)) {
            ++occurrences;
        }
        EXPECT(occurrences == 1);
        EXPECT(wire.find("Content-Length: 2\r\n") != std::string::npos);
    }

    // Issue #130: a driver whose get_connected() blocks behind an in-flight
    // connect -- the telescopes listed in async_connectable.h. SynScan
    // produced #130 and is deliberately NOT one of them any more: that fix
    // made its getter a bare atomic load. The router must poll
    // get_connecting(), the non-blocking signal, so GET connected/connecting
    // answer at once mid-connect and the PUT connected wait honours its 8 s
    // deadline instead of stalling for the whole handshake.
    {
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        using Ms = std::chrono::milliseconds;
        const auto elapsed_ms = [](std::chrono::steady_clock::time_point since) {
            return std::chrono::duration_cast<Ms>(std::chrono::steady_clock::now() - since).count();
        };

        // Platform 7 Connect returns immediately; the task then holds the
        // mutex for 1500 ms. GET connected / connecting inside that window
        // must answer at once (false / true), not after the handshake. The
        // 800 ms budget below is generous headroom over the couple of HTTP
        // dispatch + JSON round trips it actually costs — plenty under a
        // sanitizer's instrumentation overhead (ASan/TSan), while still far
        // short of the 1500 ms handshake, so a real regression back to
        // blocking on the mutex still fails this.
        auto stub = std::make_shared<LockedSlowConnectStubDriver>(9702, Ms(1500));
        EXPECT(registry.register_device(stub));
        const std::string base = "/api/v1/covercalibrator/9702";
        {
            const auto resp = route_request(router, "PUT", base + "/connect", "ClientID=1");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
        }
        std::this_thread::sleep_for(Ms(100));  // the task is inside the "handshake"
        EXPECT(stub->get_connecting());
        const auto get_started = std::chrono::steady_clock::now();
        EXPECT(!get_connected_value(router, base, "1"));
        {
            const auto resp = route_request(router, "GET", base + "/connecting");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("Value", false));
        }
        EXPECT(elapsed_ms(get_started) < 800);
        // Once the task finishes the same client reads true.
        std::this_thread::sleep_for(Ms(1800));
        EXPECT(!stub->get_connecting());
        EXPECT(get_connected_value(router, base, "1"));
        put_connected(router, base, "1", false);
        EXPECT(!stub->get_connected());

        // The PUT connected wait: its first get_connected() call used to block
        // on the driver mutex for the entire connect, so the 8 s deadline
        // never fired. With a 9.5 s handshake the reply must come back at the
        // deadline with Connecting still true, and the link comes up after.
        // (Deliberately ~10 s of wall clock: the deadline is the thing under
        // test.)
        auto slow = std::make_shared<LockedSlowConnectStubDriver>(9703, Ms(9500));
        EXPECT(registry.register_device(slow));
        const std::string slow_base = "/api/v1/covercalibrator/9703";
        const auto put_started = std::chrono::steady_clock::now();
        put_connected(router, slow_base, "1", true);
        EXPECT(elapsed_ms(put_started) < 9200);
        EXPECT(slow->get_connecting());
        std::this_thread::sleep_for(Ms(2000));
        EXPECT(!slow->get_connecting());
        EXPECT(get_connected_value(router, slow_base, "1"));
        put_connected(router, slow_base, "1", false);
        EXPECT(!slow->get_connected());
    }

    // open-astro#289: the description carries the host-clock state, and the
    // client-clock policy can be toggled and persisted through the same PUT
    // that owns Location/ProfileName.
    {
        alpacahttp::Router clock_router;
        char path_template[] = "/tmp/alpacahttp_test_routing_clock_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        ::close(fd);
        const std::string config_path = path_template;
        ::unlink(config_path.c_str());  // the router creates it on first persist
        clock_router.set_config_path(config_path);

        auto desc = [&]() {
            const auto resp = route_request(clock_router, "GET", "/management/v1/description");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
            return json["Value"];
        };
        auto v = desc();
        EXPECT(v.contains("ClockSynchronized") && v["ClockSynchronized"].is_boolean());
        EXPECT(v.contains("ClockSource") && v["ClockSource"].is_string());
        const std::string source = v["ClockSource"].get<std::string>();
        EXPECT(source == "ntp" || source == "rtc" || source == "none");  // never "client" before a UTCDate write
        EXPECT(v["ClockSynchronized"].get<bool>() == (source == "ntp"));
        EXPECT(v.value("SyncSystemClockFromClients", false) == true);
        EXPECT(clock_router.sync_system_clock_from_clients());
        // open-astro#354: the host zone rides along as an IANA name or "".
        // What it resolves to depends on the build host, so pin the payload
        // to the resolver's own answer for this host (a stubbed-out
        // desc["TimeZone"] = "" would fail on any host with a zone) and its
        // shape; the resolver's own table is the block below.
        EXPECT(v.contains("TimeZone") && v["TimeZone"].is_string());
        {
            const std::string tz = v["TimeZone"].get<std::string>();
            EXPECT(tz == alpacahttp::util::host_time_zone());
            EXPECT(tz.empty() || alpacahttp::util::looks_like_iana_zone(tz));
        }

        // Boolean and string forms are both accepted; the value persists to the config file.
        auto put = route_request(clock_router, "PUT", "/management/v1/description",
                                 R"({"SyncSystemClockFromClients": false})");
        auto put_json = nlohmann::json::parse(put.body(), nullptr, false);
        EXPECT(!put_json.is_discarded() && put_json.value("ErrorNumber", -1) == 0);
        EXPECT(!clock_router.sync_system_clock_from_clients());
        EXPECT(desc().value("SyncSystemClockFromClients", true) == false);
        {
            std::ifstream in(config_path);
            std::stringstream buf;
            buf << in.rdbuf();
            EXPECT(buf.str().find("sync_system_clock_from_clients: \"false\"") != std::string::npos);
        }
        put = route_request(clock_router, "PUT", "/management/v1/description",
                            R"({"syncSystemClockFromClients": "true"})");
        put_json = nlohmann::json::parse(put.body(), nullptr, false);
        EXPECT(!put_json.is_discarded() && put_json.value("ErrorNumber", -1) == 0);
        EXPECT(clock_router.sync_system_clock_from_clients());
        // A non-bool value is rejected and leaves the setting alone.
        put = route_request(clock_router, "PUT", "/management/v1/description", R"({"SyncSystemClockFromClients": 3})");
        put_json = nlohmann::json::parse(put.body(), nullptr, false);
        EXPECT(!put_json.is_discarded() && put_json.value("ErrorNumber", 0) != 0);
        EXPECT(clock_router.sync_system_clock_from_clients());
        ::unlink(config_path.c_str());
    }

    // Issue #358: a connect failure carries the driver's reason, not a constant.
    {
        alpacahttp::Router router;
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto refusing = std::make_shared<RefusingConnectStubDriver>(9650);
        EXPECT(registry.register_device(refusing));

        // The ASCOM path. Before this, the driver's explanation reached the
        // server log and stopped there: the client was told "Connection
        // failed" and nothing else, so every "why won't it connect" question
        // started with asking the operator for the log.
        const auto response = route_request(router, "PUT", "/api/v1/covercalibrator/9650/connected", "Connected=true");
        const auto json = nlohmann::json::parse(response.body(), nullptr, false);
        EXPECT(!json.is_discarded());
        // Error NUMBER unchanged -- a client matching on it is unaffected.
        EXPECT(json.value("ErrorNumber", 0) == static_cast<int>(alpacacore::AlpacaError::NotConnected));
        EXPECT(json.value("ErrorMessage", "") == std::string(RefusingConnectStubDriver::kReason));

        // The Platform 7 path has no slot for this: PUT /connect returns
        // success immediately by design and completion is observed through
        // Connecting, so a failed task leaves no response to carry an error.
        // The reason surfaces on the management side instead, where the web UI
        // shows it -- the only place an operator on that path can see it.
        const auto connect_response = route_request(router, "PUT", "/api/v1/covercalibrator/9650/connect", "");
        const auto connect_json = nlohmann::json::parse(connect_response.body(), nullptr, false);
        EXPECT(!connect_json.is_discarded() && connect_json.value("ErrorNumber", -1) == 0);
        for (int i = 0; i < 100 && refusing->get_connecting(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT(!refusing->get_connecting());
        EXPECT(!refusing->get_connected());

        const auto listed = nlohmann::json::parse(
            route_request(router, "GET", "/management/v1/configureddevices").body(), nullptr, false);
        EXPECT(!listed.is_discarded() && listed.contains("Value"));
        bool found_reason = false;
        for (const auto& entry : listed["Value"]) {
            if (entry.value("DeviceType", "") == "CoverCalibrator" && entry.value("DeviceNumber", -1) == 9650) {
                found_reason = entry.value("LastConnectError", "") == std::string(RefusingConnectStubDriver::kReason);
            }
        }
        EXPECT(found_reason);

        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9650);
    }

    // Issue #384: the cross-origin 403 echoes the client's transaction id.
    {
        alpacahttp::Router router;

        // Both callers of reject_cross_origin_request() had already parsed the
        // real ClientTransactionID and passed it to every other error path in
        // the same handler; only this one returned a hardcoded 0. The Alpaca
        // convention is that ClientTransactionID echoes what the client sent,
        // and clients are allowed to match responses to requests on it -- so
        // the one reply whose explanation a client most needs to surface was
        // the one reply it could not attribute.
        const auto rejected = [&router](const std::string& path, const std::string& body) {
            std::ostringstream raw;
            raw << "PUT " << path << "?ClientTransactionID=4242 HTTP/1.1\r\n"
                << "Host: localhost\r\n"
                << "Origin: http://evil.example\r\n"
                << "Content-Type: application/json\r\n"
                << "Content-Length: " << body.size() << "\r\n\r\n"
                << body;
            alpacahttp::Request request;
            EXPECT(request.parse(raw.str()));
            return router.route(request, 1);
        };

        // The utcdate setter is the third caller (added by #401, because a
        // UTCDate write can step the host clock), and it is the one whose
        // ClientTransactionID comes from dispatch_telescope_method far above
        // rather than from a line or two up -- so it is the easiest of the
        // three to leave on the hardcoded 0. It needs a registered telescope
        // to reach the setter at all.
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto cross_origin_scope = std::make_shared<TelescopeClockStubDriver>(9804);
        EXPECT(registry.register_device(cross_origin_scope));

        // The management endpoints read ClientTransactionID from the query
        // string; a device PUT reads it from the BODY (handle_device()), so
        // the utcdate case has to send it there or it would assert against a
        // 0 the fix never touches -- a test that fails for the wrong reason.
        struct Case {
            const char* path;
            const char* body;
        };
        for (const Case& c : {Case{"/management/v1/synctime", "{}"}, Case{"/management/v1/wifi/connect", "{}"},
                              Case{"/api/v1/telescope/9804/utcdate", R"({"ClientTransactionID": 4242})"}}) {
            const char* path = c.path;
            const auto response = rejected(path, c.body);
            EXPECT(response.status_code() == 403);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded());
            EXPECT(json.value("ClientTransactionID", 0U) == 4242U);
            // Still the cross-origin rejection and not some other error that
            // happens to echo the id.
            EXPECT(json.value("ErrorMessage", "").find("Cross-origin") != std::string::npos);
        }

        // A request that sends no ClientTransactionID still gets 0 back, which
        // is what the Alpaca default means -- the fix is an echo, not a
        // synthesised value.
        {
            std::ostringstream raw;
            raw << "PUT /management/v1/synctime HTTP/1.1\r\n"
                << "Host: localhost\r\n"
                << "Origin: http://evil.example\r\n"
                << "Content-Type: application/json\r\n"
                << "Content-Length: 2\r\n\r\n{}";
            alpacahttp::Request request;
            EXPECT(request.parse(raw.str()));
            const auto response = router.route(request, 1);
            EXPECT(response.status_code() == 403);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ClientTransactionID", 99U) == 0U);
        }

        registry.unregister_device(alpacacore::DeviceType::Telescope, 9804);
    }

#ifdef ALPACACORE_ENABLE_SKYWATCHER
    // Issue #388: an explicit JSON null in a device config reads as absence,
    // not as a type error.
    {
        alpacahttp::Router router;

        // `contains()` is true for an explicit null and json::value() throws
        // type_error rather than returning the default, so this used to throw
        // out of the vendor branch, get caught by the handler's outer catch,
        // and come back as an nlohmann type complaint instead of the specific
        // message the field has. Since #274/#353 these two fields decide
        // whether the device connects at all, so the difference matters.
        nlohmann::json config = {{"vendor", "skywatcher"},     {"deviceType", "telescope"}, {"deviceNumber", 0},
                                 {"connectionType", "serial"}, {"portPath", "/dev/null"},   {"siteLatitude", nullptr},
                                 {"siteLongitude", nullptr}};
        const auto response = route_request(router, "POST", "/management/v1/configuredevice", config.dump());
        const auto json = nlohmann::json::parse(response.body(), nullptr, false);
        EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        const std::string message = json.value("ErrorMessage", "");
        EXPECT(message.find("Site latitude and longitude are required") != std::string::npos);
        // The failure mode this replaces: any nlohmann type_error text.
        EXPECT(message.find("json.exception") == std::string::npos);
    }

    {
        // Issue #388, the other half: a config field of a genuinely wrong TYPE
        // is still an error -- silently falling back would accept a typo'd
        // config and register a device with defaults nobody asked for -- but
        // config_get() reports it as an AlpacaException naming the field
        // rather than letting nlohmann's type_error reach the outer catch.
        //
        // The null case above does NOT cover this: it asserts only the absence
        // of "json.exception" text, and passes with or without the try/catch,
        // because a null never reaches get<T>() at all. Deleting the catch in
        // config_get() must fail HERE.
        alpacahttp::Router router;
        nlohmann::json config = {{"vendor", "skywatcher"},  {"deviceType", "telescope"},
                                 {"deviceNumber", 50},      {"connectionType", "serial"},
                                 {"portPath", "/dev/null"}, {"siteLatitude", "-43.5"},  // a string, not a number
                                 {"siteLongitude", 172.6}};
        const auto response = route_request(router, "POST", "/management/v1/configuredevice", config.dump());
        const auto json = nlohmann::json::parse(response.body(), nullptr, false);
        EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        const std::string message = json.value("ErrorMessage", "");
        // Names the field, and says what it got.
        EXPECT(message.find("siteLatitude") != std::string::npos);
        EXPECT(message.find("wrong type") != std::string::npos);
        // The failure mode this replaces: a raw nlohmann type_error naming nothing.
        EXPECT(message.find("json.exception") == std::string::npos);
    }

    // Issue #398: site coordinates are range-checked, not just checked for
    // presence.
    {
        alpacahttp::Router router;

        // The driver already rejects exactly these through the ASCOM setters
        // (InvalidValue outside +/-90 and +/-180), so a client could not do
        // this at runtime -- only a config could. Latitude 200 reads as
        // northern to hemisphere_south_locked() and longitude 999 goes into
        // every LST computation at face value.
        int next_device_number = 40;  // clear of the devices other cases register
        const auto configure = [&router, &next_device_number](const nlohmann::json& overrides) {
            nlohmann::json config = {
                {"vendor", "skywatcher"},     {"deviceType", "telescope"}, {"deviceNumber", next_device_number++},
                {"connectionType", "serial"}, {"portPath", "/dev/null"},   {"siteLatitude", -43.5},
                {"siteLongitude", 172.6}};
            config.update(overrides);
            const auto response = route_request(router, "POST", "/management/v1/configuredevice", config.dump());
            return nlohmann::json::parse(response.body(), nullptr, false);
        };

        for (const auto& bad : {nlohmann::json{{"siteLatitude", 200.0}}, nlohmann::json{{"siteLatitude", -90.5}},
                                nlohmann::json{{"siteLongitude", 999.0}}, nlohmann::json{{"siteLongitude", -180.5}}}) {
            const auto json = configure(bad);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
            EXPECT(json.value("ErrorMessage", "").find("out of range") != std::string::npos);
        }

        // The limits are inclusive, and the poles and the antimeridian are
        // real places.
        const int first_accepted = next_device_number;
        for (const auto& edge : {nlohmann::json{{"siteLatitude", 90.0}}, nlohmann::json{{"siteLatitude", -90.0}},
                                 nlohmann::json{{"siteLongitude", 180.0}}, nlohmann::json{{"siteLongitude", -180.0}}}) {
            // A unique device number per iteration, so this is the real
            // "accepted and registered" path rather than a later
            // "device already exists" refusal that happens not to say
            // "out of range".
            const auto json = configure(edge);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
        }

        // Unregister what the accepted half just registered. These four go
        // through the real configure path, so they are also PERSISTED to
        // config/registered_devices.json -- and a second run of this binary
        // against the same working directory would then get "Device already
        // registered" from configure() and fail the EXPECT above on a device
        // that is fine. CI never saw it because it starts from a clean
        // checkout; running the suite twice locally did.
        for (int device = first_accepted; device < next_device_number; ++device) {
            remove_device(router, "skywatcher", "telescope", device);
        }
    }
#endif  // ALPACACORE_ENABLE_SKYWATCHER

    // Issue #348: every state-changing management endpoint carries the
    // cross-origin guard, not just synctime and wifi.
    {
        alpacahttp::Router router;

        // The management surface is unauthenticated by design under the
        // trusted-LAN model. The guard is what stops that stance from also
        // covering a page the operator merely has open in a browser on the
        // same LAN: a POST with Content-Type: text/plain is not preflighted,
        // and these handlers parse the body regardless of content type, so
        // nothing on the browser side would have stopped a drive-by.
        const auto request_with = [](const std::string& method, const std::string& path, const std::string& origin,
                                     const std::string& body) {
            std::ostringstream raw;
            raw << method << " " << path << "?ClientTransactionID=77 HTTP/1.1\r\n"
                << "Host: localhost\r\n";
            if (!origin.empty()) {
                raw << "Origin: " << origin << "\r\n";
            }
            // text/plain on purpose: the un-preflighted shape is the one the
            // guard exists for, and it must reach the handler all the same.
            raw << "Content-Type: text/plain\r\n"
                << "Content-Length: " << body.size() << "\r\n\r\n"
                << body;
            alpacahttp::Request request;
            EXPECT(request.parse(raw.str()));
            return request;
        };

        struct Endpoint {
            const char* method;
            const char* path;
            const char* body;
            // The endpoint's own `what` label. docs/wifi-api.md and the
            // CHANGELOG both claim "the rejection message names the
            // endpoint"; asserting only "Cross-origin" left all eight free to
            // pass the same string with the loop still green.
            const char* what;
        };
        const Endpoint endpoints[] = {
            {"PUT", "/management/v1/description", R"({"Location":"moved"})", "server description"},
            {"POST", "/management/v1/configuredevice", R"({"DeviceType":"telescope"})", "device configuration"},
            {"POST", "/management/v1/removedevice", R"({"DeviceType":"telescope","DeviceNumber":0})", "device removal"},
            {"PUT", "/management/v1/loglevel", R"({"Level":"TRACE"})", "log level"},
            {"POST", "/management/v1/shutdown", "{}", "shutdown"},
            {"POST", "/management/v1/restart", "{}", "restart"},
            {"DELETE", "/management/v1/logfiles/alpaca.log", "", "log file"},
            // The collection form deletes EVERY log file. Guarding the
            // per-file DELETE and not this one would have been exactly the
            // accidental difference the audit exists to remove.
            {"DELETE", "/management/v1/logfiles", "", "log files"},
        };

        for (const auto& ep : endpoints) {
            // A foreign origin is refused before the handler does anything.
            const auto blocked = router.route(request_with(ep.method, ep.path, "http://evil.example", ep.body), 1);
            EXPECT(blocked.status_code() == 403);
            const auto blocked_json = nlohmann::json::parse(blocked.body(), nullptr, false);
            EXPECT(!blocked_json.is_discarded());
            EXPECT(blocked_json.value("ErrorMessage", "") ==
                   std::string("Cross-origin ") + ep.what + " requests are not allowed");
            EXPECT(blocked_json.value("ClientTransactionID", 0U) == 77U);
            // The echo IS asserted: #384 landed on main before this branch
            // merged, so the shared helper now carries the client's id into
            // the 403 body instead of a hardcoded 0. request_with() sends 77,
            // and each of these endpoints reaches the helper through its own
            // handler -- so this also checks every one of them passes a real
            // client id rather than a literal, which is the mistake #384 was.

            // The same-origin portal is unaffected. What the handler then
            // does with the request is its own business -- these run against a
            // router with no shutdown/restart callback and no such device --
            // so the assertion is only that the guard did not fire.
            const auto same_origin = router.route(request_with(ep.method, ep.path, "http://localhost", ep.body), 1);
            EXPECT(same_origin.status_code() != 403);

            // A non-browser client (curl, a native app) sends no Origin at all.
            const auto no_origin = router.route(request_with(ep.method, ep.path, "", ep.body), 1);
            EXPECT(no_origin.status_code() != 403);
        }

        // GET stays exempt everywhere, so the web UI's polling keeps working
        // from any origin -- including the log viewer and the level readback,
        // whose handlers share a function with the guarded methods.
        for (const char* path : {"/management/v1/description", "/management/v1/loglevel", "/management/v1/logfiles",
                                 // The log VIEWER, not just the listing: this
                                 // is the one read path whose handler guards
                                 // ahead of all its own logic, so it is the
                                 // one most likely to lose its GET exemption.
                                 "/management/v1/logfiles/alpaca.log", "/management/v1/configureddevices"}) {
            const auto response = router.route(request_with("GET", path, "http://evil.example", ""), 1);
            EXPECT(response.status_code() != 403);
        }

        // The one documented asymmetry, pinned so docs/wifi-api.md cannot
        // drift from it: the collection's guard sits inside its DELETE
        // branch, so a cross-origin PUT here is answered by the method check
        // rather than refused, while the per-file form returns 403.
        const auto collection_put =
            router.route(request_with("PUT", "/management/v1/logfiles", "http://evil.example", ""), 1);
        EXPECT(collection_put.status_code() == 200);
        const auto collection_json = nlohmann::json::parse(collection_put.body(), nullptr, false);
        EXPECT(!collection_json.is_discarded() && collection_json.value("ErrorNumber", 0) != 0);
        const auto item_put =
            router.route(request_with("PUT", "/management/v1/logfiles/alpaca.log", "http://evil.example", ""), 1);
        EXPECT(item_put.status_code() == 403);
    }

    // Issue #444: the buildinfo endpoint the header badge reads. Nothing
    // pinned the route, the endpoint name, or the payload keys before this,
    // so a rename on either side would have been caught only by opening the
    // web UI and noticing the badge had gone quiet.
    {
        alpacahttp::Router router;

        const auto fetch_build_info = [&router](const std::string& path) {
            const auto response = route_request(router, "GET", path);
            return nlohmann::json::parse(response.body(), nullptr, false);
        };

        const auto json = fetch_build_info("/management/v1/buildinfo");
        EXPECT(!json.is_discarded());
        EXPECT(json.value("ErrorNumber", -1) == 0);
        EXPECT(json.contains("Value") && json["Value"].is_object());

        // Every key the badge reads, with the type it reads it as. The two
        // booleans matter most: GitIsRelease arriving as a string would be
        // truthy in JS for BOTH "true" and "false", which would hide the
        // badge on every build.
        const auto& value = json["Value"];
        EXPECT(value.contains("Version") && value["Version"].is_string());
        EXPECT(value.contains("GitBranch") && value["GitBranch"].is_string());
        EXPECT(value.contains("GitCommit") && value["GitCommit"].is_string());
        EXPECT(value.contains("GitRemoteUrl") && value["GitRemoteUrl"].is_string());
        EXPECT(value.contains("GitDirty") && value["GitDirty"].is_boolean());
        EXPECT(value.contains("GitIsRelease") && value["GitIsRelease"].is_boolean());
        EXPECT(value.value("Version", "") == std::string(alpacahttp::kVersion));

        // The unversioned alias resolves to the same handler.
        const auto alias = fetch_build_info("/management/buildinfo");
        EXPECT(!alias.is_discarded());
        EXPECT(alias.value("ErrorNumber", -1) == 0);
        EXPECT(alias["Value"] == value);

        // ClientTransactionID is echoed, as on every other management route.
        const auto echoed = fetch_build_info("/management/v1/buildinfo?ClientTransactionID=8271");
        EXPECT(!echoed.is_discarded());
        EXPECT(echoed.value("ClientTransactionID", 0) == 8271);
    }

    std::cout << "All routing tests passed!\n";
    return 0;
}

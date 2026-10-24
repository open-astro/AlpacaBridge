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
#include <alpacacore/util/rotator_sync_offset_store.h>
#include <alpacacore/util/thermal_camera_choice.h>
#include <alpacacore/util/unique_id.h>
#include <alpacahttp/config.h>
#include <alpacahttp/request.h>
#include <alpacahttp/router.h>
#include <alpacahttp/software_update.h>
#include <alpacahttp/util/error_mapping.h>
#include <alpacahttp/util/host_timezone.h>
#include <alpacahttp/version.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "route_table_stubs.h"
#include "test_assert.h"
#include "test_catalog_descriptor.h"

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

// open-astro#392: route_request() with a chosen Host header, or none at all
// when `host` is std::nullopt (an empty string sends an empty Host header).
alpacahttp::Response route_with_host(alpacahttp::Router& router, const std::string& method, const std::string& path,
                                     const std::optional<std::string>& host, const std::string& body = std::string()) {
    alpacahttp::Request request;
    std::ostringstream raw;
    raw << method << " " << path << " HTTP/1.1\r\n";
    if (host) {
        raw << "Host: " << *host << "\r\n";
    }
    if (!body.empty()) {
        raw << "Content-Type: application/json\r\n";
        raw << "Content-Length: " << body.size() << "\r\n";
    }
    raw << "\r\n";
    raw << body;

    EXPECT(request.parse(raw.str()));
    return router.route(request, 1);
}

// True when the response is the open-astro#392 Host refusal for `host`. The
// message names the web UI field first: a browser refused by name is where a
// person meets it, and the IP address is always allowed.
bool is_host_refusal(const alpacahttp::Response& response, const std::string& host) {
    if (response.status_code() != 403) {
        return false;
    }
    const auto json = nlohmann::json::parse(response.body(), nullptr, false);
    return !json.is_discarded() && json.value("ErrorNumber", 0) == 0x401 &&
           json.value("ErrorMessage", "") ==
               "Host '" + host +
                   "' is not allowed; open the web UI by IP address and add it under Allowed host names, or add it "
                   "to http.allowed_hosts";
}

// Issue #102 back-fill helper: POST a device config, then read it back from
// configureddevices. Returns the round-tripped Config object for
// (device_type, device_number), or a null json if configuration failed or the
// device is missing — callers EXPECT(!cfg.is_null()) first, then assert every
// persisted field survived (the automated catch for sanitize_device_config
// allowlist gaps and FormData-style key loss).
// Decision 0018: every stored config carries the stored ID (a minted GUID or a
// kept ID) in uniqueId. The config helpers below check its shape and drop it, so the many exact
// config comparisons stay about the vendor's own fields; a case about the ID
// reads it through raw_listed_config().
nlohmann::json without_unique_id(nlohmann::json config) {
    if (config.is_object() && config.contains("uniqueId")) {
        EXPECT(config["uniqueId"].is_string() &&
               alpacacore::util::is_keepable_unique_id(config["uniqueId"].get<std::string>()));
        config.erase("uniqueId");
    }
    return config;
}

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
            return without_unique_id(entry.value("Config", nlohmann::json()));
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
    int error_number = 0;
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

nlohmann::json raw_listed_config(alpacahttp::Router& router, const std::string& device_type, int device_number) {
    const auto entry = listed_entry(router, device_type, device_number);
    return entry.is_null() ? nlohmann::json() : entry.value("Config", nlohmann::json());
}

nlohmann::json listed_config(alpacahttp::Router& router, const std::string& device_type, int device_number) {
    return without_unique_id(raw_listed_config(router, device_type, device_number));
}

// The "<vendor> (failed to load)" row GET /management/v1/faileddevices lists
// for a persisted entry that did not register (LoadError true, lower-case
// DeviceType). configureddevices never lists it (open-astro#576).
nlohmann::json listed_failed_entry(alpacahttp::Router& router, const std::string& lower_type, int device_number) {
    const auto listed =
        nlohmann::json::parse(route_request(router, "GET", "/management/v1/faileddevices").body(), nullptr, false);
    if (listed.is_discarded() || !listed.contains("Value") || !listed["Value"].is_array()) {
        return nlohmann::json();
    }
    for (const auto& entry : listed["Value"]) {
        if (entry.value("LoadError", false) && entry.value("DeviceType", "") == lower_type &&
            entry.value("DeviceNumber", -1) == device_number) {
            return entry;
        }
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
    attempt.error_number = response.value("ErrorNumber", -1);
    if (attempt.error_number != 0) {
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
    bool failed_listed = false;   // not registered, but listed by faileddevices as "<vendor> (failed to load)"
    nlohmann::json failed_entry;  // that row (DeviceType lower-case, LoadError true, sanitized Config)
    std::string name;             // DeviceName of the registered device (what the factory was handed)
    std::vector<std::string> warnings;
    std::vector<std::string> errors;  // an exception out of a driver constructor is logged at ERROR, not WARN
};

// Runs in a scratch working directory of its own. The Router reads and rewrites
// the fixed relative path config/registered_devices.json, so writing the entry
// into the real one meant putting the original back on every exit path, and a
// scope guard cannot do that here: EXPECT is std::abort() (test_assert.h), no
// destructor runs, and a failure between the write and the restore left the
// entry in the real file to fail the next run with "already registered" (#657).
// With the file in a scratch directory the real one is never written, so there is
// nothing to restore on a return, a throw or an abort. The directory is entered
// only for this call, which is safe because nothing routes on another Router while
// it runs (the callers are sequential).
struct ScopedCwd {
    std::filesystem::path original = std::filesystem::current_path();
    std::filesystem::path dir = original / "persisted_attempt_cwd" / std::to_string(::getpid());
    ScopedCwd() {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);
    }
    ~ScopedCwd() {
        std::error_code ec;
        std::filesystem::current_path(original, ec);
        std::filesystem::remove_all(dir, ec);
    }
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;
};

// Write [entry] as the whole registered_devices.json of a scratch directory,
// construct a Router (which loads it with ConfigSource::Persisted), read
// configureddevices, capture every WARN and ERROR the load logged, and
// unregister the device from the process-wide DeviceRegistry. `device_type` is
// the listed name ("Telescope"); the entry carries the lower-case one.
// `extend_catalog`, when set, adds test descriptors to the Router's catalog
// before the persisted file is loaded (open-astro#664): the load runs inside
// the constructor, so a descriptor added through Router::catalog() afterwards
// is too late for the ConfigSource::Persisted path.
PersistedAttempt persisted_attempt(
    const nlohmann::json& entry, const std::string& device_type,
    const std::function<void(alpacacore::catalog::DeviceCatalog&)>& extend_catalog = {}) {
    const std::filesystem::path persisted = std::filesystem::path("config") / "registered_devices.json";
    const std::string vendor = entry.value("vendor", "");
    const std::string lower_type = entry.value("deviceType", "");
    const int device_number = entry.value("deviceNumber", -1);

    const ScopedCwd scratch;
    // Only this entry: the Router below registers EVERY entry in the file, and
    // an entry left behind by an earlier block (or a real one on a dev box)
    // would be re-registered here as a side effect.
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
    alpacahttp::Router startup_router(extend_catalog);
    {
        const auto listed = listed_entry(startup_router, device_type, device_number);
        result.config =
            listed.is_null() ? nlohmann::json() : without_unique_id(listed.value("Config", nlohmann::json()));
        result.name = listed.is_null() ? std::string() : listed.value("DeviceName", "");
    }
    result.listed = !result.config.is_null();
    if (!result.listed) {
        result.failed_entry = listed_failed_entry(startup_router, lower_type, device_number);
        result.failed_listed = !result.failed_entry.is_null();
    }
    alpacacore::logging::set_log_sink(previous_sink);

    // A device the load dropped was never registered, so remove_device() (itself an
    // EXPECT) is only called for a listed one. It saves "[]" over the scratch file only.
    if (result.listed) {
        remove_device(startup_router, vendor, lower_type, device_number);
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
    std::string fallback_unique_id() const override { return "firmware-stub-" + std::to_string(number_); }
    std::string get_description() const override { return "fake device"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 1; }
    bool get_connected() const override {
        get_connected_calls.fetch_add(1);
        if (connected_throws) {
            throw alpacacore::AlpacaException("link probe failed", alpacacore::AlpacaError::DriverException);
        }
        return connected;
    }
    bool get_connecting() const override { return connecting; }
    void set_connected(bool) override {}
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return ""; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return ""; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return ""; }

    bool connected = true;
    bool connecting = false;
    mutable std::atomic<int> get_connected_calls{0};
    bool connected_throws = false;
    std::string link_fault;
    bool link_fault_throws = false;

    std::string get_link_fault() const override {
        if (link_fault_throws) {
            throw alpacacore::AlpacaException("fault probe failed", alpacacore::AlpacaError::DriverException);
        }
        return link_fault;
    }

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
    std::string fallback_unique_id() const override { return "telescope-clock-stub-" + std::to_string(number_); }
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
    bool get_can_slew_alt_az() const override { return can_slew_flags; }
    bool get_can_slew_alt_az_async() const override { return false; }
    bool get_can_sync_alt_az() const override { return false; }
    bool get_can_slew() const override { return can_slew_flags; }
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
    void set_target_declination(double) override { ++sync_slew_calls; }
    double get_target_right_ascension() const override { return 0.0; }
    void set_target_right_ascension(double) override { ++sync_slew_calls; }
    int get_tracking_rate() const override { return 0; }
    void set_tracking_rate(int) override {}
    std::vector<int> get_tracking_rates() const override { return {}; }
    void find_home() override {}
    void park() override {}
    void pulse_guide(int, int) override {}
    void set_park() override {}
    // #775: the driver says it can slew synchronously; the router must not
    // let a client reach these three.
    bool can_slew_flags = false;
    int sync_slew_calls = 0;
    void slew_to_coordinates(double, double) override { ++sync_slew_calls; }
    void slew_to_coordinates_async(double, double) override {}
    void slew_to_target() override { ++sync_slew_calls; }
    void slew_to_target_async() override {}
    void sync_to_coordinates(double, double) override {}
    void sync_to_target() override {}
    void unpark() override {}
    bool get_can_move_axis(int) const override { return false; }
    void move_axis(int, double) override {}
    std::pair<double, double> get_axis_rate_range(int) const override { return {0.0, 0.0}; }
    void abort_slew() override {}
    void slew_to_alt_az(double, double) override { ++sync_slew_calls; }
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
    std::string fallback_unique_id() const override { return "connect-stub-" + std::to_string(number_); }
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

// Mirrors the shape the blocking-driver list in async_connectable.h guards
// against (that list is empty today; no real driver has it): an
// AsyncConnectable driver whose get_connected() takes the state mutex that
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
    std::string fallback_unique_id() const override { return "locked-slow-stub-" + std::to_string(number_); }
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
    std::string fallback_unique_id() const override { return "refusing-connect-stub-" + std::to_string(number_); }
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

// Issue #776: a driver whose synchronous connect() throws NotConnected (the
// default AlpacaDriver::connect() is set_connected(true)), counting disconnects.
class ThrowingSyncConnectStubDriver final : public alpacacore::AlpacaDriver {
public:
    static constexpr const char* kReason = "Serial port is busy: another program holds it";
    int disconnect_calls = 0;

    explicit ThrowingSyncConnectStubDriver(int number) : number_(number) {}

    int get_device_number() const override { return number_; }
    std::string get_name() const override { return "Throwing Sync Connect Stub"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::CoverCalibrator; }
    std::string fallback_unique_id() const override { return "throwing-sync-stub-" + std::to_string(number_); }
    std::string get_description() const override { return "fake device"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 1; }
    bool get_connected() const override { return false; }
    bool get_connecting() const override { return false; }
    void set_connected(bool connected) override {
        if (connected) {
            throw alpacacore::AlpacaException(kReason, alpacacore::AlpacaError::NotConnected);
        }
    }
    void disconnect() override { ++disconnect_calls; }
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

} // namespace

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

    // open-astro#711: libstdc++ std::regex recurses once per repeated
    // character, so a ~60,000-byte path segment overflowed the worker stack
    // and killed the server. route() refuses a path over 2048 bytes with 400
    // + a text/plain message before any regex, and never echoes the path back.
    {
        const auto length_refusal = [&](const std::string& path) {
            const auto resp = route_request(router, "GET", path + "?ClientTransactionID=711");
            // #1476: text/plain, no JSON envelope (and so no ClientTransactionID echo).
            const std::string message = resp.body();
            const bool refused = message.find("the limit is 2048 bytes") != std::string::npos;
            if (refused) {
                EXPECT(resp.status_code() == 400);
                EXPECT(resp.get_header("Content-Type").find("text/plain") == 0);
                EXPECT(message.find(std::to_string(path.size())) != std::string::npos);
                EXPECT(message.find("aaaa") == std::string::npos);
            }
            return refused;
        };
        const std::string long_segment(60000, 'a');
        EXPECT(length_refusal("/api/v1/" + long_segment + "/0/connected"));
        EXPECT(length_refusal("/setup/v1/" + long_segment + "/0/setup"));
        EXPECT(length_refusal("/management/v1/wifi/profiles/" + long_segment));
        EXPECT(length_refusal("/management/v1/logfiles/" + long_segment));
        EXPECT(length_refusal("/web/" + long_segment));

        // Boundary: 2048 bytes still routes, 2049 bytes is refused.
        const std::string prefix = "/api/v1/";
        const std::string suffix = "/0/connected";
        const std::string at_limit = prefix + std::string(2048 - prefix.size() - suffix.size(), 'a') + suffix;
        EXPECT(at_limit.size() == 2048);
        EXPECT(!length_refusal(at_limit));
        const std::string over_limit = prefix + std::string(2049 - prefix.size() - suffix.size(), 'a') + suffix;
        EXPECT(over_limit.size() == 2049);
        EXPECT(length_refusal(over_limit));

        // No regression on an ordinary device path: no device at 0 is still
        // the handler's 400, not the length refusal.
        EXPECT(!length_refusal("/api/v1/telescope/0/connected"));
        const auto resp = route_request(router, "GET", "/api/v1/telescope/0/connected");
        EXPECT(resp.status_code() == 400);
        EXPECT(resp.get_header("Content-Type").find("text/plain") == 0);
        EXPECT(!resp.body().empty());
    }

    // open-astro#740: a setup path the regex rejects is client input, so its
    // log line is DEBUG, not WARNING, and the path in it is cut to 256 bytes
    // plus "... (<N> bytes)". The 404 response is unchanged.
    {
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

        const std::string prefix = "/setup/v1/nonsense";
        const std::string path = prefix + std::string(1000 - prefix.size(), 'x');
        EXPECT(path.size() == 1000);
        const auto resp = route_request(router, "GET", path);
        EXPECT(resp.status_code() == 404);
        EXPECT(resp.body().find("Endpoint not found: " + path) != std::string::npos);

        const std::string tag = "Setup endpoint regex did not match: ";
        const std::string expected = tag + path.substr(0, 256) + "... (1000 bytes)";
        std::lock_guard<std::mutex> lock(captured_mutex);
        int matched = 0;
        for (const auto& line : captured) {
            EXPECT(line.level != alpacacore::logging::LogLevel::Warn);
            EXPECT(line.level != alpacacore::logging::LogLevel::Error);
            if (line.message.find(tag) != std::string::npos) {
                ++matched;
                EXPECT(line.level == alpacacore::logging::LogLevel::Debug);
                EXPECT(line.message == expected);
            }
        }
        EXPECT(matched == 1);
    }

    // Client text in a log line is escaped (open-astro#753): a request path, a
    // moveaxis body and an Accept header reach the log with control bytes and
    // invalid UTF-8 as \xNN, and the 256-byte cut never splits a multi-byte
    // character. The request line cannot carry a newline (the parser splits it
    // on whitespace), so the path uses ESC; a body and a header value can.
    {
        std::vector<std::string> captured;
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
            [&](alpacacore::logging::LogLevel, std::string_view, std::string_view message) {
                std::lock_guard<std::mutex> lock(captured_mutex);
                captured.emplace_back(message);
            });
        auto count_logged = [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(captured_mutex);
            return std::count(captured.begin(), captured.end(), line);
        };
        auto raw_byte_logged = [&](char byte) {
            std::lock_guard<std::mutex> lock(captured_mutex);
            return std::any_of(captured.begin(), captured.end(),
                               [&](const std::string& line) { return line.find(byte) != std::string::npos; });
        };

        // 255 bytes, then a 2-byte character that would end at byte 257.
        const std::string head = std::string("/setup/v1/x\x1b[31m") + "\xff";
        const std::string path = head + std::string(255 - head.size(), 'a') + "\xC3\xA9" + "tail";
        const std::string shown = "/setup/v1/x\\x1b[31m\\xff" + std::string(255 - head.size(), 'a') + "... (261 bytes)";
        EXPECT(path.size() == 261);
        EXPECT(route_request(router, "GET", path).status_code() == 404);
        EXPECT(count_logged("HTTP GET " + shown) == 1);
        EXPECT(count_logged("Handling setup endpoint: " + shown) == 1);
        EXPECT(count_logged("Setup endpoint regex did not match: " + shown) == 1);
        EXPECT(!raw_byte_logged('\x1b'));
        EXPECT(!raw_byte_logged('\xff'));
        EXPECT(!raw_byte_logged('\xC3'));

        auto& registry = alpacacore::management::DeviceRegistry::instance();
        constexpr int kStubNumber = 9753;
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::TelescopeStub>(kStubNumber)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::CameraStub>(kStubNumber)));
        auto send_raw = [&](const std::string& raw) {
            alpacahttp::Request request;
            EXPECT(request.parse(raw));
            return router.route(request, 1);
        };

        const std::string body = "Axis=0&Rate=0\nHTTP GET /forged";
        send_raw("PUT /api/v1/telescope/" + std::to_string(kStubNumber) +
                 "/moveaxis HTTP/1.1\r\nHost: localhost\r\n"
                 "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
                 std::to_string(body.size()) + "\r\n\r\n" + body);
        EXPECT(count_logged("moveaxis body: Axis=0&Rate=0\\x0aHTTP GET /forged") == 1);
        EXPECT(count_logged("HTTP GET /forged") == 0);

        send_raw("GET /api/v1/camera/" + std::to_string(kStubNumber) +
                 "/imagearray HTTP/1.1\r\nHost: localhost\r\nAccept: x\x1b[2Jy\r\n\r\n");
        EXPECT(count_logged("Camera imagearray Accept: x\\x1b[2Jy, imagebytes=false") == 1);
        EXPECT(!raw_byte_logged('\x1b'));

        // A validation message that quotes a decoded client value (the UTCDate
        // text) reaches the DEBUG log escaped too: %0A in a form value decodes
        // to a newline that would otherwise start a forged log line.
        {
            const std::string utc_body = "UTCDate=nope%0AHTTP+GET+/forged";
            send_raw("PUT /api/v1/telescope/" + std::to_string(kStubNumber) +
                     "/utcdate HTTP/1.1\r\nHost: localhost\r\n"
                     "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
                     std::to_string(utc_body.size()) + "\r\n\r\n" + utc_body);
            EXPECT(count_logged("Unparsable request parameter: Invalid UTC date format: "
                                "nope\\x0aHTTP GET /forged") == 1);
            EXPECT(!raw_byte_logged('\n'));
        }

        // configuredevice with a deviceType carrying a newline: the DEBUG
        // lines in register_device_from_config / sanitize_device_config quote
        // it and the exception text, both client text.
        {
            const std::string cfg_body =
                "{\"vendor\":\"zwo\",\"deviceType\":\"cam\\nHTTP GET /forged\","
                "\"deviceNumber\":0}";
            send_raw(
                "POST /management/v1/configuredevice HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Type: application/json\r\nContent-Length: " +
                std::to_string(cfg_body.size()) + "\r\n\r\n" + cfg_body);
            EXPECT(count_logged("register_device_from_config: device type \"cam\\x0aHTTP GET /forged\" is not "
                                "catalog-recognized (Unknown device type: cam\\x0aHTTP GET /forged); the device "
                                "is answered as not supported") == 1);
            EXPECT(!raw_byte_logged('\n'));
        }

        registry.unregister_device(alpacacore::DeviceType::Telescope, kStubNumber);
        registry.unregister_device(alpacacore::DeviceType::Camera, kStubNumber);
    }

    {
        // case: ImageArray asked too early is logged at DEBUG, a fault InvalidOperation at ERROR (#1167)
        constexpr int kCameraNumber = 8981;
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto camera = std::make_shared<route_table_stubs::CameraStub>(kCameraNumber);
        EXPECT(registry.register_device(camera));

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
        // The level of the one router line about this exception, and the client's view of it.
        auto read_image = [&](const std::string& method, const std::string& text) {
            {
                std::lock_guard<std::mutex> lock(captured_mutex);
                captured.clear();
            }
            const auto resp =
                route_request(router, "GET", "/api/v1/camera/" + std::to_string(kCameraNumber) + "/" + method);
            EXPECT(resp.status_code() == 200);
            const auto body = nlohmann::json::parse(resp.body());
            EXPECT(body.value("ErrorNumber", 0) == static_cast<int>(alpacacore::AlpacaError::InvalidOperation));
            EXPECT(body.value("ErrorMessage", "") == text);
            const std::string line = "AlpacaException in camera method '" + method + "': " + text;
            std::lock_guard<std::mutex> lock(captured_mutex);
            int matched = 0;
            alpacacore::logging::LogLevel level = alpacacore::logging::LogLevel::Trace;
            for (const auto& c : captured) {
                if (c.message == line) {
                    ++matched;
                    level = c.level;
                }
            }
            EXPECT(matched == 1);
            return level;
        };

        camera->set_image_array_error(alpacacore::AlpacaException::asked_too_early("Image not ready"));
        EXPECT(read_image("imagearray", "Image not ready") == alpacacore::logging::LogLevel::Debug);

        camera->set_image_array_error(alpacacore::AlpacaException("Exposure stopped during image retrieval",
                                                                  alpacacore::AlpacaError::InvalidOperation));
        EXPECT(read_image("imagearray", "Exposure stopped during image retrieval") ==
               alpacacore::logging::LogLevel::Error);
        camera->set_image_array_error(std::nullopt);
        registry.unregister_device(alpacacore::DeviceType::Camera, kCameraNumber);
    }

    {
        // case: ImageArrayVariant returns the Alpaca unsupported-property error without reading an image (#906)
        constexpr int kCameraNumber = 8983;
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto camera = std::make_shared<route_table_stubs::CameraStub>(kCameraNumber);
        EXPECT(registry.register_device(camera));

        for (bool connected : {false, true}) {
            camera->set_connected(connected);
            for (bool image_available : {false, true}) {
                camera->set_image_array({{1, 2, 3, 4}, 2, 2, 2});
                camera->set_image_array_error(
                    image_available ? std::nullopt
                                    : std::optional{alpacacore::AlpacaException::asked_too_early("Image not ready")});
                for (const std::string accept : {"", "application/json", "application/imagebytes"}) {
                    alpacahttp::Request request;
                    std::string raw = "GET /api/v1/camera/" + std::to_string(kCameraNumber) +
                                      "/imagearrayvariant?ClientTransactionID=906 HTTP/1.1\r\nHost: localhost\r\n";
                    if (!accept.empty()) {
                        raw += "Accept: " + accept + "\r\n";
                    }
                    raw += "\r\n";
                    EXPECT(request.parse(raw));
                    const auto response = router.route(request, 904);
                    EXPECT(response.status_code() == 200);
                    EXPECT(response.get_header("Content-Type") == "application/json");
                    EXPECT(response.get_header("Content-Length") == std::to_string(response.body().size()));
                    const auto body = nlohmann::json::parse(response.body());
                    EXPECT(body.at("ErrorNumber") == (connected ? alpacacore::AlpacaError::PropertyNotImplemented
                                                                : alpacacore::AlpacaError::NotConnected));
                    EXPECT(body.at("ClientTransactionID") == 906);
                    EXPECT(body.at("ServerTransactionID") == 904);
                    EXPECT(!body.value("ErrorMessage", "").empty());
                    if (connected) {
                        EXPECT(body.value("ErrorMessage", "").find("use ImageArray") != std::string::npos);
                    }
                    EXPECT(!body.contains("Value"));
                    EXPECT(camera->image_getter_calls == 0);
                }
            }
        }
        registry.unregister_device(alpacacore::DeviceType::Camera, kCameraNumber);
    }

    {
        // case: GET PercentCompleted outside Exposing is InvalidOperation, not the driver's raw 0 (#1477)
        constexpr int kCameraNumber = 8982;
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::CameraStub>(kCameraNumber)));
        const auto resp =
            route_request(router, "GET", "/api/v1/camera/" + std::to_string(kCameraNumber) + "/percentcompleted");
        EXPECT(resp.status_code() == 200);
        const auto body = nlohmann::json::parse(resp.body());
        EXPECT(body.value("ErrorNumber", 0) == static_cast<int>(alpacacore::AlpacaError::InvalidOperation));
        registry.unregister_device(alpacacore::DeviceType::Camera, kCameraNumber);
    }

    {
        // case: ImageBytes preserves metadata, ordering, and element bytes
        constexpr int kCameraNumber = 8980;
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto camera = std::make_shared<route_table_stubs::CameraStub>(kCameraNumber);
        EXPECT(registry.register_device(camera));

        auto request_image = [&](const std::string& accept) {
            alpacahttp::Request request;
            std::string raw = "GET /api/v1/camera/" + std::to_string(kCameraNumber) +
                              "/imagearray?ClientTransactionID=77 HTTP/1.1\r\nHost: localhost\r\n";
            if (!accept.empty()) {
                raw += "Accept: " + accept + "\r\n";
            }
            raw += "\r\n";
            EXPECT(request.parse(raw));
            return router.route(request, 1);
        };
        auto check_imagebytes = [&](alpacacore::ImageArray image, std::uint32_t transmission_type, std::uint32_t rank,
                                    std::uint32_t width, std::uint32_t height, const std::string& pixels) {
            camera->set_image_array(image);
            const auto response = request_image("application/imagebytes");
            const auto& body = response.body();
            EXPECT(response.get_header("Content-Type") == "application/imagebytes");
            EXPECT(response.get_header("Content-Length") == std::to_string(body.size()));
            EXPECT(body.size() == 44 + pixels.size());
            auto read_u32 = [&](std::size_t offset) {
                return static_cast<std::uint32_t>(static_cast<unsigned char>(body[offset])) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(body[offset + 1])) << 8) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(body[offset + 2])) << 16) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(body[offset + 3])) << 24);
            };
            EXPECT(read_u32(0) == 1);
            EXPECT(read_u32(4) == 0);
            EXPECT(read_u32(8) == 77);
            EXPECT(read_u32(12) == 1);
            EXPECT(read_u32(16) == 44);
            EXPECT(read_u32(20) == 2);
            EXPECT(read_u32(24) == transmission_type);
            EXPECT(read_u32(28) == rank);
            EXPECT(read_u32(32) == width);
            EXPECT(read_u32(36) == height);
            EXPECT(read_u32(40) == (rank == 3 ? 3 : 0));
            EXPECT(body.substr(44) == pixels);
        };
        auto append_u16 = [](std::string& bytes, std::uint16_t value) {
            bytes.push_back(static_cast<char>(value & 0xff));
            bytes.push_back(static_cast<char>(value >> 8));
        };
        auto append_i32 = [](std::string& bytes, std::int32_t value) {
            const auto bits = static_cast<std::uint32_t>(value);
            for (unsigned shift = 0; shift < 32; shift += 8) {
                bytes.push_back(static_cast<char>((bits >> shift) & 0xff));
            }
        };
        auto bytes = [](std::initializer_list<std::uint8_t> values) {
            std::string result;
            for (const auto value : values) {
                result.push_back(static_cast<char>(value));
            }
            return result;
        };

        check_imagebytes({{0, 1, 2, 3, 4, 5}, 3, 2, 2}, 6, 2, 3, 2, bytes({0, 3, 1, 4, 2, 5}));
        check_imagebytes({{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, 2, 2, 3}, 6, 3, 2, 2,
                         bytes({1, 2, 3, 7, 8, 9, 4, 5, 6, 10, 11, 12}));

        std::string uint16_pixels;
        for (std::uint16_t value : {0, 256, 65535}) {
            append_u16(uint16_pixels, value);
        }
        check_imagebytes({{0, 256, 65535}, 3, 1, 2}, 8, 2, 3, 1, uint16_pixels);

        std::string int16_pixels;
        for (std::int16_t value : {-32768, 0, -1, 1, 32767, 2}) {
            append_u16(int16_pixels, static_cast<std::uint16_t>(value));
        }
        check_imagebytes({{-32768, -1, 32767, 0, 1, 2}, 3, 2, 2}, 1, 2, 3, 2, int16_pixels);

        std::string int32_pixels;
        append_i32(int32_pixels, std::numeric_limits<std::int32_t>::min());
        append_i32(int32_pixels, std::numeric_limits<std::int32_t>::max());
        check_imagebytes(
            {{std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()}, 2, 1, 2}, 2, 2, 2, 1,
            int32_pixels);

        alpacacore::ImageArray edge_image;
        edge_image.width = 65;
        edge_image.height = 67;
        edge_image.rank = 2;
        for (std::int32_t i = 0; i < edge_image.width * edge_image.height; ++i) {
            edge_image.data.push_back(i - 2177);
        }
        std::string edge_pixels;
        for (int x = 0; x < edge_image.width; ++x) {
            for (int y = 0; y < edge_image.height; ++y) {
                auto source = edge_image.data[static_cast<std::size_t>(y) * edge_image.width + x];
                append_u16(edge_pixels, static_cast<std::uint16_t>(static_cast<std::int16_t>(source)));
            }
        }
        check_imagebytes(edge_image, 1, 2, 65, 67, edge_pixels);

        camera->set_image_array({{0, 1, 2, 3, 4, 5}, 3, 2, 2});
        const auto json_response = request_image("");
        EXPECT(json_response.get_header("Content-Type") == "application/json");
        const auto json = nlohmann::json::parse(json_response.body());
        EXPECT(json["Value"] == nlohmann::json::array({nlohmann::json::array({0, 3}), nlohmann::json::array({1, 4}),
                                                       nlohmann::json::array({2, 5})}));

        auto expect_invalid_image = [&](alpacacore::ImageArray image) {
            camera->set_image_array(std::move(image));
            for (const std::string& accept : {std::string{}, std::string{"application/imagebytes"}}) {
                const auto response = request_image(accept);
                EXPECT(response.get_header("Content-Type") == "application/json");
                const auto error = nlohmann::json::parse(response.body());
                EXPECT(error.value("ErrorNumber", 0) == alpacacore::AlpacaError::DriverException);
                EXPECT(error.value("ErrorMessage", "").find("Camera returned invalid image data") != std::string::npos);
            }
        };
        expect_invalid_image({{1}, 2, 1, 2});
        expect_invalid_image({{1}, 0, 1, 2});
        expect_invalid_image({{1}, 1, 1, 0});
        expect_invalid_image({{}, std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), 3});

        registry.unregister_device(alpacacore::DeviceType::Camera, kCameraNumber);
    }

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
    test_request = "PUT /api/v1/telescope/0/slewtocoordinates?RightAscension=1.5&Declination=-20.3 HTTP/1.1\r\n\r\n";
    EXPECT(request.parse(test_request));
    EXPECT(request.path() == "/api/v1/telescope/0/slewtocoordinates");
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
    // must be 400 (Bad Request), not 404, with a text/plain message (#1476).
    {
        // Misspelt / unknown device type.
        const auto resp = route_request(router, "GET", "/api/v1/wibble/0/connected");
        EXPECT(resp.status_code() == 400);
        EXPECT(resp.get_header("Content-Type").find("text/plain") == 0);  // #1476
        EXPECT(!resp.body().empty());
    }
    {
        // Valid device type, unknown method.
        const auto resp = route_request(router, "GET", "/api/v1/telescope/0/notarealmethod");
        EXPECT(resp.status_code() == 400);
        EXPECT(resp.get_header("Content-Type").find("text/plain") == 0);  // #1476
        EXPECT(!resp.body().empty());
    }
    {
        // Valid type and method, but no device registered at that number.
        const auto resp = route_request(router, "GET", "/api/v1/telescope/4242/connected");
        EXPECT(resp.status_code() == 400);
        EXPECT(resp.get_header("Content-Type").find("text/plain") == 0);  // #1476
        EXPECT(!resp.body().empty());
    }

    {
        // case: telescope synchronous slews are MethodNotImplemented (#775)
        // ASCOM's slew FAQ: a synchronous slew blocks past the client's
        // timeout, so CanSlew and CanSlewAltAz read false for every telescope
        // and the three synchronous slew calls raise 0x400 before any
        // parameter parse, target write or driver call. Async forms stay.
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto scope = std::make_shared<TelescopeClockStubDriver>(9775);
        scope->can_slew_flags = true;
        EXPECT(registry.register_device(scope));
        const std::string base = "/api/v1/telescope/9775";
        for (const char* getter : {"/canslew", "/canslewaltaz"}) {
            const auto resp = route_request(router, "GET", base + getter);
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
            EXPECT(json.contains("Value") && json["Value"].is_boolean() && json["Value"].get<bool>() == false);
        }
        for (const char* path : {"/slewtocoordinates?RightAscension=5&Declination=10", "/slewtotarget",
                                 "/slewtoaltaz?Altitude=45&Azimuth=90", "/slewtocoordinates"}) {
            const auto resp = route_request(router, "PUT", base + path);
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) == 0x400);
        }
        EXPECT(scope->sync_slew_calls == 0);
        for (const char* path : {"/slewtocoordinatesasync?RightAscension=5&Declination=10", "/slewtotargetasync",
                                 "/slewtoaltazasync?Altitude=45&Azimuth=90"}) {
            const auto resp = route_request(router, "PUT", base + path);
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
        }
        registry.unregister_device(alpacacore::DeviceType::Telescope, 9775);
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
            EXPECT(resp.status_code() == 400);  // #1476: Axis missing is not interpretable
            EXPECT(resp.get_header("Content-Type").find("text/plain") == 0);
        }
        {
            const auto resp = route_request(router, "GET", base + "/axisrates");
            EXPECT(resp.status_code() == 400);  // #1476: Axis missing is not interpretable
            EXPECT(resp.get_header("Content-Type").find("text/plain") == 0);
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

        // 1. Non-finite / hex-float doubles are rejected (HTTP 400 text, #1476)
        // before reaching the driver's setter.
        for (const std::string& raw : {"nan", "inf", "-infinity", "0x1p3"}) {
            const auto resp =
                route_request(router, "PUT", base + "/targetdeclination", "TargetDeclination=" + raw + "&ClientID=1");
            // #1476: it does not parse as an Alpaca number, so HTTP 400 text.
            EXPECT(resp.status_code() == 400);
            EXPECT(resp.get_header("Content-Type").find("text/plain") == 0);
            EXPECT(resp.body().find("TargetDeclination") != std::string::npos);
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
            EXPECT(resp.get_header("Content-Type").find("text/plain") == 0);
            EXPECT(!resp.body().empty());
        }

        // 4. #627: a valid uint32 device number above INT_MAX names no device
        // (the registry keys devices by int). It gets the ordinary not-found
        // reply naming the number as sent, never a lookup of the negative int
        // a narrowing cast would give (4294967295 -> -1, 2147483648 ->
        // INT_MIN). Stubs sit at both negative numbers, so the old cast
        // found one and answered 200.
        EXPECT(registry.register_device(std::make_shared<TelescopeClockStubDriver>(-1)));
        EXPECT(registry.register_device(std::make_shared<TelescopeClockStubDriver>(std::numeric_limits<int>::min())));
        EXPECT(registry.get_device(alpacacore::DeviceType::Telescope, -1) != nullptr);
        EXPECT(registry.get_device(alpacacore::DeviceType::Telescope, std::numeric_limits<int>::min()) != nullptr);
        for (const std::string number : {"4294967295", "2147483648"}) {
            const auto resp = route_request(router, "GET", "/api/v1/telescope/" + number + "/connected");
            EXPECT(resp.status_code() == 400);
            EXPECT(resp.get_header("Content-Type").find("text/plain") == 0);
            EXPECT(resp.body() == "Device not found: telescope #" + number);
        }
        registry.unregister_device(alpacacore::DeviceType::Telescope, -1);
        registry.unregister_device(alpacacore::DeviceType::Telescope, std::numeric_limits<int>::min());
    }

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
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_device = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Telescope" &&
                entry.value("DeviceNumber", -1) == 9101) {
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
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_celestron = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Telescope" &&
                entry.value("DeviceNumber", -1) == 9102) {
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
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);

        nlohmann::json remove_net_body = {
            {"vendor", "celestron"},
            {"deviceType", "telescope"},
            {"deviceNumber", 9103}
        };
        (void)route_request(router, "POST", "/management/v1/removedevice", remove_net_body.dump());
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
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
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_touptek = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Camera" &&
                entry.value("DeviceNumber", -1) == 9201) {
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
        EXPECT(remove_json.value("ErrorNumber", -1) == 0);
#else
        EXPECT(configure_json.value("ErrorNumber", 0) != 0);
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
        EXPECT(configure_json.value("ErrorNumber", -1) == 0);

        const auto configured_response = route_request(router, "GET", "/management/v1/configureddevices");
        const auto configured_json = nlohmann::json::parse(configured_response.body());
        EXPECT(configured_json.value("ErrorNumber", -1) == 0);
        EXPECT(configured_json.contains("Value"));
        EXPECT(configured_json["Value"].is_array());

        bool found_touptek_focuser = false;
        for (const auto& entry : configured_json["Value"]) {
            if (entry.value("DeviceType", "") == "Focuser" &&
                entry.value("DeviceNumber", -1) == 9202) {
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

        // MaxBrightness is a property: the registered cover is not connected, so the read answers NotConnected.
        const auto maxbright_response = route_request(router, "GET", "/api/v1/covercalibrator/9401/maxbrightness");
        const auto maxbright_json = nlohmann::json::parse(maxbright_response.body());
        EXPECT(maxbright_json.value("ErrorNumber", -1) == 0x407);

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
        // case: zwo device selectors are range-checked before narrowing
        constexpr std::uint64_t kTwoTo32 = std::uint64_t{1} << 32;
        struct SelectorCase {
            const char* device_type;
            const char* field;
            const char* other_field;
        };
        constexpr SelectorCase selectors[]{{"camera", "cameraIndex", "cameraId"},
                                           {"camera", "cameraId", "cameraIndex"},
                                           {"filterwheel", "filterwheelIndex", "filterwheelId"},
                                           {"filterwheel", "filterwheelId", "filterwheelIndex"},
                                           {"focuser", "focuserIndex", "focuserId"},
                                           {"focuser", "focuserId", "focuserIndex"},
                                           {"rotator", "rotatorIndex", "rotatorId"},
                                           {"rotator", "rotatorId", "rotatorIndex"},
                                           {"switch", "cameraIndex", "cameraId"},
                                           {"switch", "cameraId", "cameraIndex"}};
        int device_number = 9620;
        for (const auto& selector : selectors) {
            nlohmann::json body = {{"vendor", "zwo"},
                                   {"deviceType", selector.device_type},
                                   {"deviceNumber", device_number++},
                                   {selector.other_field, 0}};
            body[selector.field] = kTwoTo32 + 1;
            const auto result = nlohmann::json::parse(
                route_request(router, "POST", "/management/v1/configuredevice", body.dump()).body());
            EXPECT(result.value("ErrorNumber", 0) == 0x401);
            EXPECT(result.value("ErrorMessage", std::string()).find(selector.field) != std::string::npos);
        }
        for (const char* field : {"cameraIndex", "cameraId"}) {
            nlohmann::json body = {{"vendor", "zwo"},
                                   {"deviceType", "camera"},
                                   {"deviceNumber", 9620},
                                   {"cameraIndex", 0},
                                   {"cameraId", 7}};
            body[field] = -2;
            const auto result = nlohmann::json::parse(
                route_request(router, "POST", "/management/v1/configuredevice", body.dump()).body());
            EXPECT(result.value("ErrorNumber", 0) == 0x401);
            EXPECT(result.value("ErrorMessage", std::string()).find(field) != std::string::npos);
            // The camera keeps the arm's text for an out-of-range selector.
            EXPECT(result.value("ErrorMessage", std::string()) ==
                   std::string("Invalid value for ") + field + ": expected an integer from -1 to 2147483647");
        }
        for (const auto& [field, value, other_field] :
             {std::tuple{"cameraIndex", 0.0, "cameraId"}, std::tuple{"cameraId", 7.0, "cameraIndex"}}) {
            const int number = device_number++;
            nlohmann::json body = {{"vendor", "zwo"},
                                   {"deviceType", "camera"},
                                   {"deviceNumber", number},
                                   {field, value},
                                   {other_field, -1}};
            const auto result = nlohmann::json::parse(
                route_request(router, "POST", "/management/v1/configuredevice", body.dump()).body());
            EXPECT(result.value("ErrorNumber", -1) == 0);
            const auto saved = listed_config(router, "Camera", number);
            EXPECT(saved.contains(field));
            EXPECT(saved.at(field).get<double>() == value);
            remove_device(router, "zwo", "camera", number);
        }

        const nlohmann::json max_index_config = {{"vendor", "zwo"},
                                                 {"deviceType", "camera"},
                                                 {"deviceNumber", 9630},
                                                 {"cameraIndex", std::numeric_limits<int>::max()}};
        const auto max_index = nlohmann::json::parse(
            route_request(router, "POST", "/management/v1/configuredevice", max_index_config.dump()).body());
        EXPECT(max_index.value("ErrorNumber", -1) == 0);
        remove_device(router, "zwo", "camera", 9630);

        for (const auto& [camera_index, camera_id, number] : {std::tuple{-1, 7, 9631}, std::tuple{0, -1, 9632}}) {
            const nlohmann::json sentinel_config = {{"vendor", "zwo"},
                                                    {"deviceType", "camera"},
                                                    {"deviceNumber", number},
                                                    {"cameraIndex", camera_index},
                                                    {"cameraId", camera_id}};
            const auto result = nlohmann::json::parse(
                route_request(router, "POST", "/management/v1/configuredevice", sentinel_config.dump()).body());
            EXPECT(result.value("ErrorNumber", -1) == 0);
            remove_device(router, "zwo", "camera", number);
        }
    }
    {
        // case: zwo persisted out-of-range selectors are dropped without loading a different unit
        constexpr std::uint64_t kWrappedIndex = (std::uint64_t{1} << 32) + 1;
        struct PersistedSelectorCase {
            const char* device_type;
            const char* listed_type;
            const char* field;
        };
        constexpr PersistedSelectorCase selectors[]{{"camera", "Camera", "cameraIndex"},
                                                    {"camera", "Camera", "cameraId"},
                                                    {"filterwheel", "FilterWheel", "filterwheelIndex"},
                                                    {"filterwheel", "FilterWheel", "filterwheelId"},
                                                    {"focuser", "Focuser", "focuserIndex"},
                                                    {"focuser", "Focuser", "focuserId"},
                                                    {"rotator", "Rotator", "rotatorIndex"},
                                                    {"rotator", "Rotator", "rotatorId"},
                                                    {"switch", "Switch", "cameraIndex"},
                                                    {"switch", "Switch", "cameraId"}};
        int device_number = 9621;
        for (const auto& selector : selectors) {
            nlohmann::json entry = {
                {"vendor", "zwo"}, {"deviceType", selector.device_type}, {"deviceNumber", device_number}};
            entry[selector.field] = kWrappedIndex;
            const auto persisted = persisted_attempt(entry, selector.listed_type);
            if (!persisted.listed) {
                std::cerr << "Persisted ZWO selector failed to register: " << selector.device_type << "/"
                          << selector.field << " row=" << persisted.failed_entry.dump() << "\n";
                for (const auto& warning : persisted.warnings) std::cerr << "  warning: " << warning << "\n";
                for (const auto& error : persisted.errors) std::cerr << "  error: " << error << "\n";
            }
            EXPECT(persisted.listed);
            EXPECT(!persisted.config.contains(selector.field));
            EXPECT(any_warning_contains(persisted.warnings, selector.field));
            ++device_number;
        }
        const auto integral_float = persisted_attempt(
            {{"vendor", "zwo"}, {"deviceType", "camera"}, {"deviceNumber", device_number}, {"cameraIndex", 0.0}},
            "Camera");
        EXPECT(integral_float.listed);
        EXPECT(integral_float.config.contains("cameraIndex"));
        EXPECT(integral_float.config.at("cameraIndex").get<double>() == 0.0);
        const auto inapplicable_default = persisted_attempt({{"vendor", "zwo"},
                                                             {"deviceType", "switch"},
                                                             {"deviceNumber", device_number + 1},
                                                             {"switchType", "asiair"},
                                                             {"cameraIndex", -1}},
                                                            "Switch");
        EXPECT(inapplicable_default.listed);
        EXPECT(inapplicable_default.config.value("cameraIndex", 0) == -1);

        const ScopedCwd scratch;
        const std::filesystem::path persisted_file = std::filesystem::path("config") / "registered_devices.json";
        constexpr PersistedSelectorCase restart_cases[]{
            PersistedSelectorCase{"camera", "Camera", "cameraIndex"},
            PersistedSelectorCase{"filterwheel", "FilterWheel", "filterwheelIndex"},
            PersistedSelectorCase{"focuser", "Focuser", "focuserIndex"},
            PersistedSelectorCase{"rotator", "Rotator", "rotatorIndex"},
            PersistedSelectorCase{"switch", "Switch", "cameraIndex"},
        };
        nlohmann::json initial_entries = nlohmann::json::array();
        device_number = 9640;
        for (const auto& selector : restart_cases) {
            nlohmann::json entry = {{"vendor", "zwo"},
                                    {"deviceType", selector.device_type},
                                    {"deviceNumber", device_number++},
                                    {selector.field, kWrappedIndex}};
            initial_entries.push_back(entry);
        }
        std::filesystem::create_directories(persisted_file.parent_path());
        const auto write_entries = [&](const nlohmann::json& entries) {
            std::ofstream out(persisted_file, std::ios::trunc);
            out << entries.dump();
        };
        write_entries(initial_entries);

        nlohmann::json normalized_entries;
        {
            alpacahttp::Router first_startup;
            device_number = 9640;
            for (const auto& selector : restart_cases) {
                const auto row = listed_entry(first_startup, selector.listed_type, device_number++);
                EXPECT(!row.is_null());
                EXPECT(!row.value("LoadError", false));
            }
            std::ifstream in(persisted_file);
            in >> normalized_entries;
            device_number = 9640;
            for (const auto& selector : restart_cases) {
                remove_device(first_startup, "zwo", selector.device_type, device_number++);
            }
        }
        write_entries(normalized_entries);
        {
            alpacahttp::Router second_startup;
            device_number = 9640;
            for (const auto& selector : restart_cases) {
                const auto row = listed_entry(second_startup, selector.listed_type, device_number++);
                EXPECT(!row.is_null());
                EXPECT(!row.value("LoadError", false));
                EXPECT(!row.value("Config", nlohmann::json::object()).contains(selector.field));
                if (std::string_view(selector.device_type) == "camera") {
                    const auto connect = nlohmann::json::parse(
                        route_request(second_startup, "PUT", "/api/v1/camera/9640/connected", "Connected=true").body());
                    EXPECT(connect.value("ErrorNumber", 0) != 0);
                }
            }
            device_number = 9640;
            for (const auto& selector : restart_cases) {
                remove_device(second_startup, "zwo", selector.device_type, device_number++);
            }
        }
    }
    {
        // case: zwo persisted selectors that are not integers are dropped and the entry still loads
        // The arm dropped a stored "1" or true or 1.5 and registered the camera; the typed read
        // refuses the wrong type, so the router drops the key first and forgets it in the file.
        struct BadSelector {
            const char* device_type;
            const char* listed_type;
            const char* field;
        };
        constexpr BadSelector selectors[]{{"camera", "Camera", "cameraIndex"},
                                          {"camera", "Camera", "cameraId"},
                                          {"filterwheel", "FilterWheel", "filterwheelIndex"},
                                          {"focuser", "Focuser", "focuserId"},
                                          {"rotator", "Rotator", "rotatorIndex"},
                                          {"switch", "Switch", "cameraId"}};
        int device_number = 9680;
        for (const auto& selector : selectors) {
            for (const nlohmann::json& bad : {nlohmann::json("1"), nlohmann::json(true), nlohmann::json(1.5)}) {
                nlohmann::json entry = {
                    {"vendor", "zwo"}, {"deviceType", selector.device_type}, {"deviceNumber", device_number++}};
                entry[selector.field] = bad;
                const auto persisted = persisted_attempt(entry, selector.listed_type);
                EXPECT(persisted.listed);
                EXPECT(!persisted.config.contains(selector.field));
                EXPECT(any_warning_contains(persisted.warnings, selector.field));
            }
        }
        // An API request with such a selector is still refused.
        const nlohmann::json api_body = {
            {"vendor", "zwo"}, {"deviceType", "camera"}, {"deviceNumber", device_number}, {"cameraIndex", "1"}};
        const auto refused = nlohmann::json::parse(
            route_request(router, "POST", "/management/v1/configuredevice", api_body.dump()).body());
        EXPECT(refused.value("ErrorNumber", 0) == 0x401);
        EXPECT(refused.value("ErrorMessage", std::string()).find("cameraIndex") != std::string::npos);
    }
    {
        // case: zwo camera identity keys round trip and refuse control bytes (#914)
        // The form resends the learned identity after remove + add.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "zwo"},
                                           {"deviceType", "camera"},
                                           {"deviceNumber", 9608},
                                           {"cameraIndex", 1},
                                           {"cameraId", 7},
                                           {"serialNumber", "0c190e111d020900"},
                                           {"cameraName", "ZWO ASI533MC Pro"}},
                                          "Camera", 9608);
        EXPECT(cfg.value("serialNumber", std::string()) == "0c190e111d020900");
        EXPECT(cfg.value("cameraName", std::string()) == "ZWO ASI533MC Pro");
        remove_device(router, "zwo", "camera", 9608);

        for (const char* field : {"serialNumber", "cameraName"}) {
            nlohmann::json bad = {{"vendor", "zwo"},
                                  {"deviceType", "camera"},
                                  {"deviceNumber", 9608},
                                  {"cameraIndex", 1},
                                  {field, "0c19\nforged log line"}};
            const auto response = route_request(router, "POST", "/management/v1/configuredevice", bad.dump());
            const auto json = nlohmann::json::parse(response.body());
            EXPECT(json.value("ErrorNumber", 0) == 0x401);
            EXPECT(json.value("ErrorMessage", std::string()).find(field) != std::string::npos);
        }
        nlohmann::json not_hex = {{"vendor", "zwo"},
                                  {"deviceType", "camera"},
                                  {"deviceNumber", 9608},
                                  {"cameraIndex", 1},
                                  {"serialNumber", "not-hex!"}};
        const auto not_hex_json = nlohmann::json::parse(
            route_request(router, "POST", "/management/v1/configuredevice", not_hex.dump()).body());
        EXPECT(not_hex_json.value("ErrorNumber", 0) == 0x401);
    }
    {
        // case: zwo camera refuses a serial another entry holds (#914)
        const auto configure = [&](int number, const char* field, const char* value) {
            nlohmann::json body = {{"vendor", "zwo"},
                                   {"deviceType", "camera"},
                                   {"deviceNumber", number},
                                   {"cameraIndex", number - 9600},
                                   {field, value}};
            return nlohmann::json::parse(
                route_request(router, "POST", "/management/v1/configuredevice", body.dump()).body());
        };
        EXPECT(configure(9608, "serialNumber", "0c190e111d020900").value("ErrorNumber", -1) == 0);
        const auto dup_serial = configure(9609, "serialNumber", "0c190e111d020900");
        EXPECT(dup_serial.value("ErrorNumber", 0) == 0x401);
        EXPECT(dup_serial.value("ErrorMessage", std::string()).find("serialNumber") != std::string::npos);
        remove_device(router, "zwo", "camera", 9608);
        EXPECT(configure(9609, "serialNumber", "0c190e111d020900").value("ErrorNumber", -1) == 0);
        remove_device(router, "zwo", "camera", 9609);
    }
    {
        // case: zwo camera stored entries with a bad or duplicate identity stay listed (#914)
        // The API refuses invalid identity fields; persisted invalid fields are dropped, and a
        // selector-less camera remains registered until an operator reconfigures it.
        const ScopedCwd scratch;
        const std::filesystem::path file = std::filesystem::path("config") / "registered_devices.json";
        std::filesystem::create_directories(file.parent_path());
        {
            std::ofstream out(file, std::ios::trunc);
            out << nlohmann::json::array({{{"vendor", "zwo"},
                                           {"deviceType", "camera"},
                                           {"deviceNumber", 9712},
                                           {"cameraIndex", 0},
                                           {"serialNumber", "not-hex!"},
                                           {"cameraName", "ZWO ASI120MM Mini"}},
                                          {{"vendor", "zwo"},
                                           {"deviceType", "camera"},
                                           {"deviceNumber", 9713},
                                           {"cameraIndex", 1},
                                           {"serialNumber", "0c190e111d020900"},
                                           {"uniqueId", "ZWO_UID_00112233445566ff"}},
                                          {{"vendor", "zwo"},
                                           {"deviceType", "camera"},
                                           {"deviceNumber", 9714},
                                           {"cameraIndex", 2},
                                           {"serialNumber", "0c190e111d020900"},
                                           {"uniqueId", "ZWO_UID_00112233445566ff"}}})
                       .dump();
        }
        std::vector<std::string> first_ids;
        {
            alpacahttp::Router stored;
            for (const int number : {9713, 9714}) {
                EXPECT(!listed_entry(stored, "Camera", number).is_null());
                first_ids.push_back(raw_listed_config(stored, "Camera", number).value("uniqueId", std::string()));
            }
            EXPECT(!listed_entry(stored, "Camera", 9712).is_null());
            // Decision 0018: a stored ZWO_UID_ is kept; the later entry holding the same value
            // gets a minted GUID at load, written back.
            EXPECT(first_ids[0] == "ZWO_UID_00112233445566ff");
            EXPECT(alpacacore::util::is_unique_id(first_ids[1]));
            EXPECT(alpacacore::util::is_unique_id(
                raw_listed_config(stored, "Camera", 9712).value("uniqueId", std::string())));
            EXPECT(first_ids[0] != first_ids[1]);
            // Two stored entries sharing a serial: the first drops it, the second keeps it.
            EXPECT(listed_config(stored, "Camera", 9713).value("serialNumber", std::string()).empty());
            EXPECT(listed_config(stored, "Camera", 9714).value("serialNumber", std::string()) == "0c190e111d020900");
        }
        {
            // A second start over the same file reports the same ids.
            alpacahttp::Router again;
            EXPECT(raw_listed_config(again, "Camera", 9713).value("uniqueId", std::string()) == first_ids[0]);
            EXPECT(raw_listed_config(again, "Camera", 9714).value("uniqueId", std::string()) == first_ids[1]);
            for (const int number : {9712, 9713, 9714}) {
                remove_device(again, "zwo", "camera", number);
            }
        }
        {
            // A selector-less persisted entry stays listed; editing it removes the
            // unresolved driver first, and the form resends its saved GUID (decision 0018).
            {
                std::ofstream out(file, std::ios::trunc);
                out << nlohmann::json::array({{{"vendor", "zwo"},
                                               {"deviceType", "camera"},
                                               {"deviceNumber", 9715},
                                               {"uniqueId", "ZWO_UID_0011223344556677"}}})
                           .dump();
            }
            alpacahttp::Router unresolved;
            const auto listed = listed_entry(unresolved, "Camera", 9715);
            EXPECT(!listed.is_null());
            const std::string saved_id =
                listed.value("Config", nlohmann::json::object()).value("uniqueId", std::string());
            EXPECT(saved_id == "ZWO_UID_0011223344556677");  // a stored ZWO_UID_ is kept (decision 0018)
            remove_device(unresolved, "zwo", "camera", 9715);

            const nlohmann::json body = {{"vendor", "zwo"},
                                         {"deviceType", "camera"},
                                         {"deviceNumber", 9715},
                                         {"cameraIndex", 1},
                                         {"uniqueId", saved_id}};
            const auto reply = nlohmann::json::parse(
                route_request(unresolved, "POST", "/management/v1/configuredevice", body.dump()).body());
            EXPECT(reply.value("ErrorNumber", -1) == 0);
            EXPECT(raw_listed_config(unresolved, "Camera", 9715).value("uniqueId", std::string()) == saved_id);
            remove_device(unresolved, "zwo", "camera", 9715);
        }
        // Falsified by: dropping the uniqueId write-back in load_persisted_devices (the GUIDs are
        // not stored and differ on the second load) or clearing only the serial of a pair.
        // Falsified by: making the Persisted arm of check_identity_field or the duplicate block return
        // false (the entry would be skipped and not listed).
    }
    {
        // case: a stored entry that fails to load still holds its GUID against a new device (#1471)
        // A hand-edited numeric serialNumber fails that entry's own load; its GUID is
        // still taken, so a second camera sending the same value gets its own.
        const ScopedCwd scratch;
        const std::filesystem::path file = std::filesystem::path("config") / "registered_devices.json";
        std::filesystem::create_directories(file.parent_path());
        const std::string held = alpacacore::util::generate_unique_id();
        {
            std::ofstream out(file, std::ios::trunc);
            out << nlohmann::json::array({{{"vendor", "zwo"},
                                           {"deviceType", "camera"},
                                           {"deviceNumber", 9716},
                                           {"cameraIndex", 0},
                                           {"serialNumber", 5},
                                           {"uniqueId", held}}})
                       .dump();
        }
        alpacahttp::Router stored;
        EXPECT(listed_entry(stored, "Camera", 9716).is_null());
        const nlohmann::json body = {{"vendor", "zwo"},
                                     {"deviceType", "camera"},
                                     {"deviceNumber", 9717},
                                     {"cameraIndex", 1},
                                     {"uniqueId", held}};
        const auto reply =
            nlohmann::json::parse(route_request(stored, "POST", "/management/v1/configuredevice", body.dump()).body());
        EXPECT(reply.value("ErrorNumber", -1) == 0);
        const std::string given = raw_listed_config(stored, "Camera", 9717).value("uniqueId", std::string());
        EXPECT(alpacacore::util::is_unique_id(given));
        EXPECT(given != held);
        remove_device(stored, "zwo", "camera", 9717);
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

        // Mirror lock-up settle and long-exposure NR survive the save (ADR 0004 catalog fields).
        const auto cfg_mlu = roundtrip_config(router,
                                              {{"vendor", "gphoto"},
                                               {"deviceType", "camera"},
                                               {"deviceNumber", 9621},
                                               {"cameraIndex", 1},
                                               {"mirrorLockupSettleMs", 3500},
                                               {"longExposureNR", "on"}},
                                              "Camera", 9621);
        EXPECT(cfg_mlu.is_object() && !cfg_mlu.empty());
        EXPECT(cfg_mlu.value("mirrorLockupSettleMs", -1) == 3500);
        EXPECT(cfg_mlu.value("longExposureNR", "") == "on");
        remove_device(router, "gphoto", "camera", 9621);
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

#ifdef ALPACACORE_ENABLE_ALTAIR
    {
        // case: altair camera cameraIndex survives the config round-trip
        // altair / camera — cameraIndex survives the catalog-sanitized save.
        // Construction runs one enumeration (a failure is logged), nothing is
        // opened until connect, so it registers with no camera attached.
        const auto cfg = roundtrip_config(
            router, {{"vendor", "altair"}, {"deviceType", "camera"}, {"deviceNumber", 9666}, {"cameraIndex", 2}},
            "Camera", 9666);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("cameraIndex", -1) == 2);
        remove_device(router, "altair", "camera", 9666);
    }
#endif

#ifdef ALPACACORE_ENABLE_ASTROASIS
    {
        // astroasis / focuser — explicit hidPath persists through
        // sanitize_device_config. (An empty hidPath instead falls back to
        // focuserIndex, whose USB scan runs at connect time since #659.)
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "astroasis"},
                                           {"deviceType", "focuser"},
                                           {"deviceNumber", 9621},
                                           {"hidPath", "/dev/hidraw3"},
                                           {"responseTimeoutMs", 2500}},
                                          "Focuser", 9621);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("hidPath", "") == "/dev/hidraw3");
        // A vendor-agnostic key survives the catalog-sanitized save.
        EXPECT(cfg.value("responseTimeoutMs", -1) == 2500);
        remove_device(router, "astroasis", "focuser", 9621);
    }
    {
        // astroasis / focuser, focuserIndex form (#659): the by-index factory
        // no longer scans the USB bus at construction, so a persisted
        // auto-detect device registers with no hardware attached instead of
        // becoming "(failed to load)", and focuserIndex survives the round-trip.
        const auto cfg = roundtrip_config(
            router, {{"vendor", "astroasis"}, {"deviceType", "focuser"}, {"deviceNumber", 9622}, {"focuserIndex", 1}},
            "Focuser", 9622);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("focuserIndex", -1) == 1);
        EXPECT(cfg.value("hidPath", "") == "");
        remove_device(router, "astroasis", "focuser", 9622);
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
        // case: iguider camera devicePath round-trip
        // The iOptron iGuider guide camera, its own catalog vendor: devicePath
        // survives the sanitizer.
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "iguider"},
                                           {"deviceType", "camera"},
                                           {"deviceNumber", 9625},
                                           {"devicePath", "/dev/v4l/by-id/usb-iOptron_iGuider_External-video-index0"}},
                                          "Camera", 9625);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("vendor", "") == "iguider");
        EXPECT(cfg.value("devicePath", "") == "/dev/v4l/by-id/usb-iOptron_iGuider_External-video-index0");
        remove_device(router, "iguider", "camera", 9625);
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
        // sanitize_device_config; the catalog keeps every field the wheel
        // declares, so the integrated wheel's cameraIndex stays in the entry
        // (ADR 0004), and an undeclared key is dropped.
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
        EXPECT(cfg.value("cameraIndex", -1) == 3);
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
                                           {"siteLatitude", 51.477833},
                                           {"siteLongitude", -0.0015},
                                           {"siteElevation", 42.0},
                                           {"mountIndex", 1}},
                                          "Telescope", 9617);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("connectionType", "") == "serial");
        EXPECT(cfg.value("portPath", "") == "/dev/ttyUSB6");
        EXPECT(cfg.value("baudRate", -1) == 9600);
        EXPECT(cfg.value("siteLatitude", 0.0) == 51.477833);
        EXPECT(cfg.value("siteLongitude", 0.0) == -0.0015);
        EXPECT(cfg.value("siteElevation", 0.0) == 42.0);
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
        // open-astro#436: the two motion limits are persisted per device and
        // must survive the sanitize round-trip (Required Test Case 9). Both
        // are OFF by default: absent or null reads back as absent-or-null,
        // never as 0, because 0 is a real floor (the horizon) and a real
        // meridian limit (stop at the meridian).
        const auto cfg = roundtrip_config(router,
                                          {{"vendor", "skywatcher"},
                                           {"deviceType", "telescope"},
                                           {"deviceNumber", 9662},
                                           {"connectionType", "network"},
                                           {"host", "192.168.4.1"},
                                           {"udpPort", 11880},
                                           {"siteLatitude", 51.477833},
                                           {"siteLongitude", -0.0015},
                                           {"minAltitudeDeg", 15.0},
                                           {"meridianLimitMinutes", 30.0}},
                                          "Telescope", 9662);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("minAltitudeDeg", -1.0) == 15.0);
        EXPECT(cfg.value("meridianLimitMinutes", -1.0) == 30.0);
        remove_device(router, "skywatcher", "telescope", 9662);

        // null = off, and the web UI sends null for a blank field: the config
        // is accepted, the device registers, and the field reads back as
        // absent or null (either means off) rather than a number.
        const auto off = roundtrip_config(router,
                                          {{"vendor", "skywatcher"},
                                           {"deviceType", "telescope"},
                                           {"deviceNumber", 9663},
                                           {"connectionType", "network"},
                                           {"host", "192.168.4.1"},
                                           {"udpPort", 11880},
                                           {"siteLatitude", 51.477833},
                                           {"siteLongitude", -0.0015},
                                           {"minAltitudeDeg", nullptr},
                                           {"meridianLimitMinutes", nullptr}},
                                          "Telescope", 9663);
        EXPECT(off.is_object() && !off.empty());
        EXPECT(!off.contains("minAltitudeDeg") || off["minAltitudeDeg"].is_null());
        EXPECT(!off.contains("meridianLimitMinutes") || off["meridianLimitMinutes"].is_null());
        remove_device(router, "skywatcher", "telescope", 9663);

        // One limit set, the other left out: only the set one is persisted.
        const auto one = roundtrip_config(router,
                                          {{"vendor", "skywatcher"},
                                           {"deviceType", "telescope"},
                                           {"deviceNumber", 9664},
                                           {"connectionType", "serial"},
                                           {"portPath", "/dev/ttyUSB6"},
                                           {"siteLatitude", 51.477833},
                                           {"siteLongitude", -0.0015},
                                           {"minAltitudeDeg", 12.5}},
                                          "Telescope", 9664);
        EXPECT(one.is_object() && !one.empty());
        EXPECT(one.value("minAltitudeDeg", -1.0) == 12.5);
        EXPECT(!one.contains("meridianLimitMinutes") || one["meridianLimitMinutes"].is_null());
        remove_device(router, "skywatcher", "telescope", 9664);

        // Range: the API (not only the form) rejects a floor outside
        // -90..90 and a meridian limit outside 0..360, and takes the bounds.
        const auto limits_body = [](int number, const char* key, double value) {
            return nlohmann::json{{"vendor", "skywatcher"},      {"deviceType", "telescope"}, {"deviceNumber", number},
                                  {"connectionType", "network"}, {"host", "192.168.4.1"},     {"udpPort", 11880},
                                  {"siteLatitude", 51.477833},   {"siteLongitude", -0.0015},  {key, value}};
        };
        const auto rejected = [&](int number, const char* key, double value) {
            const auto response =
                route_request(router, "POST", "/management/v1/configuredevice", limits_body(number, key, value).dump());
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        };
        rejected(9665, "minAltitudeDeg", 95.0);
        rejected(9665, "minAltitudeDeg", -91.0);
        rejected(9665, "meridianLimitMinutes", -1.0);
        rejected(9665, "meridianLimitMinutes", 361.0);
        const auto accepted = [&](int number, const char* key, double value) {
            const auto cfg = roundtrip_config(router, limits_body(number, key, value), "Telescope", number);
            EXPECT(cfg.is_object() && !cfg.empty());
            EXPECT(cfg.value(key, -1000.0) == value);
            remove_device(router, "skywatcher", "telescope", number);
        };
        accepted(9666, "minAltitudeDeg", -90.0);
        accepted(9666, "minAltitudeDeg", 90.0);
        accepted(9666, "meridianLimitMinutes", 0.0);
        accepted(9666, "meridianLimitMinutes", 360.0);
    }
    {
        // case: skywatcher decAxisSense config round trip
        // open-astro#582: the per-device dec axis sense survives the sanitize
        // round-trip (Required Test Case 9), and the API refuses an unknown value.
        for (const char* sense : {"normal", "reversed"}) {
            const int number = std::string(sense) == "normal" ? 9667 : 9668;
            const auto cfg = roundtrip_config(router,
                                              {{"vendor", "skywatcher"},
                                               {"deviceType", "telescope"},
                                               {"deviceNumber", number},
                                               {"connectionType", "network"},
                                               {"host", "192.168.4.1"},
                                               {"udpPort", 11880},
                                               {"siteLatitude", 51.477833},
                                               {"siteLongitude", -0.0015},
                                               {"decAxisSense", sense}},
                                              "Telescope", number);
            EXPECT(cfg.is_object() && !cfg.empty());
            EXPECT(cfg.value("decAxisSense", "") == sense);
            remove_device(router, "skywatcher", "telescope", number);
        }
        const nlohmann::json bogus{
            {"vendor", "skywatcher"},      {"deviceType", "telescope"}, {"deviceNumber", 9669},
            {"connectionType", "network"}, {"host", "192.168.4.1"},     {"udpPort", 11880},
            {"siteLatitude", 51.477833},   {"siteLongitude", -0.0015},  {"decAxisSense", "bogus"}};
        const auto response = route_request(router, "POST", "/management/v1/configuredevice", bogus.dump());
        const auto json = nlohmann::json::parse(response.body(), nullptr, false);
        EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
    }
    {
        // case: skywatcher backlash config round trip
        // open-astro#437: the four backlash keys survive the sanitize round-trip
        // (Required Test Case 9), default to 0/off when absent, and the API
        // refuses a step count outside 0..100000.
        const nlohmann::json base{
            {"vendor", "skywatcher"},     {"deviceType", "telescope"}, {"connectionType", "network"},
            {"host", "192.0.2.10"},       {"udpPort", 11880},          {"siteLatitude", -33.856784},
            {"siteLongitude", 151.215297}};
        auto with = [&](int number, const nlohmann::json& extra) {
            nlohmann::json body = base;
            body["deviceNumber"] = number;
            body.update(extra);
            return body;
        };
        const auto cfg = roundtrip_config(router,
                                          with(9670, {{"backlashStepsRA", 120},
                                                      {"backlashEnabledRA", true},
                                                      {"backlashStepsDec", 100000},
                                                      {"backlashEnabledDec", false}}),
                                          "Telescope", 9670);
        EXPECT(cfg.is_object() && !cfg.empty());
        EXPECT(cfg.value("backlashStepsRA", -1) == 120);
        EXPECT(cfg.value("backlashEnabledRA", false) == true);
        EXPECT(cfg.value("backlashStepsDec", -1) == 100000);
        EXPECT(cfg.value("backlashEnabledDec", true) == false);
        remove_device(router, "skywatcher", "telescope", 9670);

        const auto absent = roundtrip_config(router, with(9671, nlohmann::json::object()), "Telescope", 9671);
        EXPECT(absent.is_object() && !absent.empty());
        // Absent stays absent (or reads as the default): nothing is invented for a saved config.
        EXPECT(absent.value("backlashStepsRA", 0) == 0);
        EXPECT(absent.value("backlashEnabledRA", false) == false);
        EXPECT(absent.value("backlashStepsDec", 0) == 0);
        EXPECT(absent.value("backlashEnabledDec", false) == false);
        remove_device(router, "skywatcher", "telescope", 9671);

        for (const int bad : {-1, 100001}) {
            const auto response = route_request(router, "POST", "/management/v1/configuredevice",
                                                with(9672, {{"backlashStepsDec", bad}}).dump());
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        }
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
        // as unparsable (HTTP 400 text since #1476) before the setter runs, so
        // over HTTP it can no longer reach the persistence hook; the surveyed
        // value stays on disk.
        {
            const auto resp = route_request(router, "PUT", base + "/sitelatitude", "SiteLatitude=nan&ClientID=1");
            EXPECT(resp.status_code() == 400);
            EXPECT(resp.get_header("Content-Type").find("text/plain") == 0);
            EXPECT(resp.body().find("SiteLatitude") != std::string::npos);
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
        surveyed["siteLatitude"] = 51.477833;
        surveyed["siteLongitude"] = -0.0015;
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
            EXPECT(std::fabs(cfg.value("siteLatitude", 0.0) - 51.477833) < 1e-9);
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
        // The unregistrable 9655 row is served by faileddevices (#576); the
        // float-numbered skywatcher entry is registered (configureddevices) on
        // a build with that vendor and a failed row on one without it.
        const auto failed_listed = nlohmann::json::parse(
            route_request(startup_router, "GET", "/management/v1/faileddevices").body(), nullptr, false);
        bool listed_lowercase = false;
        bool float_listed = false;
        for (const auto* source : {&listed, &failed_listed}) {
            if (source->is_discarded() || !source->contains("Value") || !(*source)["Value"].is_array()) {
                continue;
            }
            for (const auto& entry : (*source)["Value"]) {
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
        // that vendor's driver (construction is hardware-free for every arm
        // since #659, but an SDK-index vendor still opens its SDK). Appending
        // would make this case depend on every earlier block having removed
        // what it added, which nothing enforces. The original contents are
        // restored below.
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
        // device, so registration alone proves nothing. SkyWatcher's site
        // getters answer without a connection, and since decision 0019 a
        // coordinate that was never set reads as InvalidOperation, so the clear
        // is observable here: the cleared latitude is unset, not 200.
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
        // NOT reach the driver: the latitude reads as never set.
        EXPECT(!lat_json.is_discarded() &&
               lat_json.value("ErrorNumber", -1) == static_cast<int>(alpacacore::AlpacaError::InvalidOperation));
        EXPECT(lat_json.value("ErrorMessage", std::string()) == "SiteLatitude has not been set");
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
                           {"siteLatitude", 51.477833}});
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9632},
                           {"connectionType", "serial"},
                           {"portPath", "/dev/ttyUSB9"},
                           {"baudRate", 9600},
                           {"siteLongitude", -0.0015}});
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
        const std::string refusal = "; it connects but refuses to point until the site is set";
        for (const auto& w : warnings) {
            if (w.find("telescope 9631 has no site longitude" + refusal) != std::string::npos) {
                lat_only = true;
            }
            if (w.find("telescope 9632 has no site latitude" + refusal) != std::string::npos) {
                lon_only = true;
            }
        }
        EXPECT(lat_only);
        EXPECT(lon_only);
    }
    {
        // case: persisted deviceType with a control byte is escaped in the log
        // sanitize_device_config() logs the deviceType of an entry the catalog
        // does not recognise, and only the persisted-config load reaches it with
        // text the file's author chose: a control byte must be logged as \xNN.
        // Scratch working directory: the real config is never written.
        const ScopedCwd scratch;
        const std::filesystem::path persisted = std::filesystem::path("config") / "registered_devices.json";
        std::filesystem::create_directories(persisted.parent_path());
        {
            std::ofstream out(persisted, std::ios::trunc);
            out << nlohmann::json::array({{{"vendor", "bisque"},
                                           {"deviceType", std::string("tele\x1b[31mscope")},
                                           {"deviceNumber", 9711}}})
                       .dump();
        }
        std::vector<std::string> captured;
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
            [&](alpacacore::logging::LogLevel, std::string_view, std::string_view message) {
                std::lock_guard<std::mutex> lock(captured_mutex);
                captured.emplace_back(message);
            });
        {
            alpacahttp::Router startup_router;
            route_request(startup_router, "GET", "/management/v1/configureddevices");
        }
        alpacacore::logging::set_log_sink(logging_restore.sink);
        bool escaped_logged = false;
        bool raw_logged = false;
        {
            std::lock_guard<std::mutex> lock(captured_mutex);
            for (const auto& line : captured) {
                if (line.find("sanitize_device_config: device type \"tele\\x1b[31mscope\"") != std::string::npos) {
                    escaped_logged = true;
                }
                if (line.find('\x1b') != std::string::npos) {
                    raw_logged = true;
                    std::cerr << "raw ESC in log line: " << line << "\n";
                }
            }
        }
        EXPECT(escaped_logged);
        EXPECT(!raw_logged);
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
                           {"siteLatitude", 51.477833},
                           {"siteLongitude", -0.0015}});
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9641},
                           {"connectionType", "network"},
                           {"host", ""},
                           {"siteLatitude", 51.477833},
                           {"siteLongitude", -0.0015}});
        entries.push_back({{"vendor", "skywatcher"},
                           {"deviceType", "telescope"},
                           {"deviceNumber", 9642},
                           {"connectionType", "carrier-pigeon"},
                           {"portPath", "/dev/ttyUSB8"},
                           {"baudRate", 9600},
                           {"siteLatitude", 51.477833},
                           {"siteLongitude", -0.0015}});
#ifdef ALPACACORE_ENABLE_ZWO
        // An entry with no connectionType key: the off-UI path (hand-edited,
        // or written by a non-web-UI client, since the form always sets the
        // field). ZWO reads it as auto like the other mount arms (#508 item 3),
        // so it registers and logs no connectionType WARN.
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
        bool warned_zwo_empty = false;  // must stay false: empty is auto (#508 item 3)
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
            // bare "serial". ZWO must NOT emit it for an empty type.
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
        EXPECT(!warned_zwo_empty);
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
                                     {"siteLatitude", 51.477833},
                                     {"siteLongitude", -0.0015}};
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
#ifdef ALPACACORE_ENABLE_CELESTRON
        // The focus motor has no auto-detect, so the API refuses it without a port (issue #896).
        reject({{"vendor", "celestron"}, {"deviceType", "focuser"}, {"deviceNumber", 9648}, {"portPath", ""}},
               "Serial port path is required for the Celestron focus motor");
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
    // Since #659 no "auto", empty-connectionType or by-index arm touches
    // hardware while the driver is constructed: the auto paths of the
    // ioptron, synscan, skywatcher, onstep and celestron mounts, the ioptron
    // network auto-scan, the by-index paths of the ioptron, gemini, qhy and
    // astroasis focusers and of the qhy cfw3 wheel all hand the driver a
    // connect-time resolver, so they register with nothing attached and are
    // round-tripped below as "auto" variants. That is also the router-level
    // regression check for the #659 symptom ("(failed to load)" at start-up):
    // a factory that scans at construction again throws out of
    // register_device_from_config() here. The scan itself still runs only
    // inside Connected=true, which nothing in this test issues.
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
            R"("responseTimeoutMs":4000,"apertureDiameter":0.2,"focalLength":1.0,"siteLatitude":51.477833,)"
            R"("siteLongitude":-0.0015,"siteElevation":42.0,"learnSiteFromClient":true,"syncTimeOnConnect":false)";
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
        // open-astro#1152: maxStep survives the save; left out it stays absent (no default is filled in).
        add("zwo", "focuser", "Focuser", "maxStep", R"({"focuserIndex":1,"maxStep":40000})",
            R"({"focuserIndex":1,"maxStep":40000})");
        add("zwo", "rotator", "Rotator", "", R"({"rotatorIndex":1,"rotatorId":2})",
            R"({"rotatorIndex":1,"rotatorId":2})");
        add("zwo", "switch", "Switch", "dewheater", R"({"switchType":"dewheater","cameraIndex":1,"cameraId":4})",
            R"({"switchType":"dewheater","cameraIndex":1,"cameraId":4})");
        // Auto (-1) and a camera bound by serial, as the camera picker saves them.
        add("zwo", "switch", "Switch", "dewheater auto", R"({"switchType":"dewheater","cameraIndex":-1})",
            R"({"switchType":"dewheater","cameraIndex":-1})");
        add("zwo", "switch", "Switch", "dewheater serial",
            R"({"switchType":"dewheater","cameraIndex":1,"serialNumber":"2e18870f28000900"})",
            R"({"switchType":"dewheater","cameraIndex":1,"serialNumber":"2e18870f28000900"})");
        // Declared fields survive whatever the switchType (ADR 0004).
        add("zwo", "switch", "Switch", "asiair",
            R"({"switchType":"asiair","gpioChip":"/dev/gpiochip0","devicePath":"/dev/x","pwmFrequencyHz":200,)"
            R"("ports":[{"gpio":12,"name":"Mount","pwm":false},{"gpio":13,"name":"Dew","pwm":true}]})",
            R"({"switchType":"asiair","gpioChip":"/dev/gpiochip0","devicePath":"/dev/x","pwmFrequencyHz":200,)"
            R"("ports":[{"gpio":12,"name":"Mount","pwm":false},{"gpio":13,"name":"Dew","pwm":true}]})");
        add("zwo", "switch", "Switch", "asiair-plus-picm4",
            R"({"switchType":"asiair-plus-picm4","gpioChip":"/dev/gpiochip0","devicePath":"/dev/x","pwmFrequencyHz":200,)"
            R"("ports":[{"gpio":12,"name":"Mount","pwm":false}]})",
            R"({"switchType":"asiair-plus-picm4","gpioChip":"/dev/gpiochip0","devicePath":"/dev/x","pwmFrequencyHz":200,)"
            R"("ports":[{"gpio":12,"name":"Mount","pwm":false}]})");
        add("zwo", "switch", "Switch", "asiair-plus-rk3568",
            R"({"switchType":"asiair-plus-rk3568","gpioChip":"/dev/gpiochip0","devicePath":"/dev/pwm-gpio-misc",)"
            R"("pwmFrequencyHz":50,"ports":[{"name":"DC1","pwm":true}]})",
            R"({"switchType":"asiair-plus-rk3568","gpioChip":"/dev/gpiochip0","devicePath":"/dev/pwm-gpio-misc",)"
            R"("pwmFrequencyHz":50,)"
            R"("ports":[{"name":"DC1","pwm":true}]})");
        // zwo / telescope had no round-trip case at all before #647.
        add("zwo", "telescope", "Telescope", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB1","baudRate":9600,"host":"h","tcpPort":1,"cameraIndex":9})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB1","baudRate":9600,"host":"h","tcpPort":1})");
        add("zwo", "telescope", "Telescope", "network",
            R"({"connectionType":"network","host":"192.168.4.1","tcpPort":4030,"portPath":"/dev/x","baudRate":9600})",
            R"({"connectionType":"network","host":"192.168.4.1","tcpPort":4030,"portPath":"/dev/x","baudRate":9600})");
        add("zwo", "telescope", "Telescope", "auto", R"({"connectionType":"auto","portPath":"/dev/x"})",
            R"({"connectionType":"auto","portPath":"/dev/x"})");
#endif

#ifdef ALPACACORE_ENABLE_QHY
        add("qhy", "camera", "Camera", "", R"({"cameraIndex":1,"cameraId":"QHY-TEST-1","bogusKey":1})",
            R"({"cameraIndex":1,"cameraId":"QHY-TEST-1"})");
        add("qhy", "focuser", "Focuser", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyACM3","focuserIndex":1,"maxStep":30000,"reverse":true,)"
            R"("speed":4,"holdForce":true,"holdIhold":6,"holdIrun":12,"temperatureSource":"chip","cameraIndex":7})",
            R"({"connectionType":"serial","portPath":"/dev/ttyACM3","focuserIndex":1,"maxStep":30000,"reverse":true,)"
            R"("speed":4,"holdForce":true,"holdIhold":6,"holdIrun":12,"temperatureSource":"chip"})");
        add("qhy", "focuser", "Focuser", "auto", R"({"connectionType":"auto","focuserIndex":1})",
            R"({"connectionType":"auto","focuserIndex":1})");  // #659
        add("qhy", "filterwheel", "FilterWheel", "integrated",
            R"({"wheelType":"integrated","cameraIndex":3,"cameraId":"QHY-CFW-1","filterNames":["L","R"],)"
            R"("connectionType":"serial","portPath":"/dev/x","filterwheelIndex":4})",
            R"({"wheelType":"integrated","cameraIndex":3,"cameraId":"QHY-CFW-1","filterNames":["L","R"],)"
            R"("connectionType":"serial","portPath":"/dev/x","filterwheelIndex":4})");
        add("qhy", "filterwheel", "FilterWheel", "cfw3-usb",
            R"({"wheelType":"cfw3-usb","connectionType":"serial","portPath":"/dev/ttyUSB7","filterwheelIndex":1,)"
            R"("filterNames":["L","R","G"],"cameraIndex":3,"cameraId":"x"})",
            R"({"wheelType":"cfw3-usb","connectionType":"serial","portPath":"/dev/ttyUSB7","filterwheelIndex":1,)"
            R"("filterNames":["L","R","G"],"cameraIndex":3,"cameraId":"x"})");
        add("qhy", "filterwheel", "FilterWheel", "cfw3-usb auto",
            R"({"wheelType":"cfw3-usb","connectionType":"auto","filterwheelIndex":1,"filterNames":["L","R","G"]})",
            R"({"wheelType":"cfw3-usb","connectionType":"auto","filterwheelIndex":1,"filterNames":["L","R","G"]})");  // #659
#endif

#ifdef ALPACACORE_ENABLE_SVBONY
        add("svbony", "camera", "Camera", "", R"({"cameraIndex":2,"cameraId":"x"})", R"({"cameraIndex":2})");
#endif
#ifdef ALPACACORE_ENABLE_GPHOTO
        add("gphoto", "camera", "Camera", "", R"({"cameraIndex":1,"cameraId":"x"})", R"({"cameraIndex":1})");
        add("gphoto", "camera", "Camera", "MLU and LENR",
            R"({"cameraIndex":1,"mirrorLockupSettleMs":3500,"longExposureNR":"on","bogusKey":1})",
            R"({"cameraIndex":1,"mirrorLockupSettleMs":3500,"longExposureNR":"on"})");
#endif

#ifdef ALPACACORE_ENABLE_TOUPTEK
        // touptek camera, focuser and filterwheel had no roundtrip_config() case before #647.
        // The catalog sanitize keeps the declared fields of each type (ADR 0004): the camera no longer
        // keeps the focuser keys and the focuser no longer keeps cameraIndex.
        add("touptek", "camera", "Camera", "", R"({"cameraIndex":1,"focuserIndex":5,"focuserId":"z"})",
            R"({"cameraIndex":1})");
        add("touptek", "focuser", "Focuser", "", R"({"focuserIndex":2,"focuserId":"AAF-1","cameraIndex":3})",
            R"({"focuserIndex":2,"focuserId":"AAF-1"})");
        add("touptek", "filterwheel", "FilterWheel", "",
            R"({"filterwheelIndex":1,"filterwheelId":"AFW-1","filterNames":["L","R","G","B","Ha"],"cameraIndex":3})",
            R"({"filterwheelIndex":1,"filterwheelId":"AFW-1","filterNames":["L","R","G","B","Ha"]})");
        add("touptek", "switch", "Switch", "thermal", R"({"switchType":"thermal","cameraIndex":2,"gpioChip":"/dev/x"})",
            R"({"switchType":"thermal","cameraIndex":2,"gpioChip":"/dev/x"})");
        add("touptek", "switch", "Switch", "thermal auto", R"({"switchType":"thermal","cameraIndex":-1})",
            R"({"switchType":"thermal","cameraIndex":-1})");
#ifdef ALPACACORE_TOUPTEK_STELLAVITA
        add("touptek", "switch", "Switch", "stellavita",
            R"({"switchType":"stellavita","gpioChip":"/dev/gpiochip0","pwmFrequencyHz":100,"cameraIndex":2,)"
            R"("ports":[{"name":"Flat Panel","pwm":true},{"name":"Camera","pwm":false}]})",
            R"({"switchType":"stellavita","gpioChip":"/dev/gpiochip0","pwmFrequencyHz":100,"cameraIndex":2,)"
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
        add("playerone", "switch", "Switch", "auto + serial", R"({"cameraIndex":-1,"serialNumber":"POA0123456"})",
            R"({"cameraIndex":-1,"serialNumber":"POA0123456"})");
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
            with_tail(
                R"("connectionType":"serial","portPath":"/dev/ttyUSB6","baudRate":115200,"mountIndex":1,"host":"h")")
                .c_str());
        // The catalog sanitize keeps every declared field whatever the connection type (ADR 0004).
        add("ioptron", "telescope", "Telescope", "network",
            R"({"connectionType":"network","host":"192.168.1.9","tcpPort":4030,"mountIndex":2,"portPath":"/dev/x"})",
            R"({"connectionType":"network","host":"192.168.1.9","tcpPort":4030,"mountIndex":2,"portPath":"/dev/x"})");
        // #659: the auto arms construct without a scan, so they register here.
        add("ioptron", "telescope", "Telescope", "auto", R"({"connectionType":"auto","mountIndex":1})",
            R"({"connectionType":"auto","mountIndex":1})");
        add("ioptron", "telescope", "Telescope", "network auto-scan",
            R"({"connectionType":"network","tcpPort":4030,"mountIndex":1})",
            R"({"connectionType":"network","tcpPort":4030,"mountIndex":1})");
        add("ioptron", "focuser", "Focuser", "auto", R"({"connectionType":"auto","focuserIndex":1,"model":"ieaf"})",
            R"({"connectionType":"auto","focuserIndex":1,"model":"ieaf"})");
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
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB5","baudRate":9600,"synscanVersion":"v4","mountIndex":2,"host":"h"})");
        add("synscan", "telescope", "Telescope", "network",
            R"({"connectionType":"network","host":"192.168.1.5","tcpPort":11880,"synscanVersion":"v3","portPath":"/dev/x"})",
            R"({"connectionType":"network","host":"192.168.1.5","tcpPort":11880,"synscanVersion":"v3","portPath":"/dev/x"})");
        add("synscan", "telescope", "Telescope", "auto",
            R"({"connectionType":"auto","synscanVersion":"v4","mountIndex":1})",
            R"({"connectionType":"auto","synscanVersion":"v4","mountIndex":1})");  // #659
        // #860: a known alignmentMode survives; an unknown string or a non-string drops.
        add("synscan", "telescope", "Telescope", "alignmentMode equatorial",
            R"({"connectionType":"auto","mountIndex":1,"alignmentMode":"equatorial"})",
            R"({"connectionType":"auto","mountIndex":1,"alignmentMode":"equatorial"})");
        add("synscan", "telescope", "Telescope", "alignmentMode altaz",
            R"({"connectionType":"auto","mountIndex":1,"alignmentMode":"altaz"})",
            R"({"connectionType":"auto","mountIndex":1,"alignmentMode":"altaz"})");
        add("synscan", "telescope", "Telescope", "alignmentMode unknown",
            R"({"connectionType":"auto","mountIndex":1,"alignmentMode":"wedge"})",
            R"({"connectionType":"auto","mountIndex":1})");
#endif

#ifdef ALPACACORE_ENABLE_SKYWATCHER
        // Site coordinates are mandatory for this vendor from the API (#274).
        // open-astro#744 rule 8: the catalog's sanitize keeps every declared
        // non-secret field whatever the connection type (ADR 0004; only the UI
        // honours applies_when), so a serial config now keeps host/udpPort
        // and a network config keeps portPath. tcpPort is not a Sky-Watcher
        // field and still drops.
        add("skywatcher", "telescope", "Telescope", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB6","baudRate":9600,"siteLatitude":51.477833,)"
            R"("siteLongitude":-0.0015,"siteElevation":42.0,"mountIndex":1,"host":"h","udpPort":1})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB6","baudRate":9600,"siteLatitude":51.477833,)"
            R"("siteLongitude":-0.0015,"siteElevation":42.0,"mountIndex":1,"host":"h","udpPort":1})");
        add("skywatcher", "telescope", "Telescope", "network",
            R"({"connectionType":"network","host":"192.168.4.1","udpPort":11880,"siteLatitude":-33.87,)"
            R"("siteLongitude":151.21,"portPath":"/dev/x","tcpPort":1})",
            R"({"connectionType":"network","host":"192.168.4.1","udpPort":11880,"siteLatitude":-33.87,)"
            R"("siteLongitude":151.21,"portPath":"/dev/x"})");
        add("skywatcher", "telescope", "Telescope", "auto",
            R"({"connectionType":"auto","mountIndex":1,"siteLatitude":51.477833,"siteLongitude":-0.0015})",
            R"({"connectionType":"auto","mountIndex":1,"siteLatitude":51.477833,"siteLongitude":-0.0015})");  // #659
#endif

#ifdef ALPACACORE_ENABLE_ONSTEP
        add("onstep", "telescope", "Telescope", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyACM0","baudRate":9600,"mountIndex":1,"host":"h"})",
            R"({"connectionType":"serial","portPath":"/dev/ttyACM0","baudRate":9600,"mountIndex":1})");
        add("onstep", "telescope", "Telescope", "auto", R"({"connectionType":"auto","mountIndex":1})",
            R"({"connectionType":"auto","mountIndex":1})");  // #659
#endif

#ifdef ALPACACORE_ENABLE_CELESTRON
        // celestron / telescope had no roundtrip_config() case before #647. It is a
        // catalog vendor since the Celestron slice: sanitize keeps every declared
        // field whatever the connection type (ADR 0004), so the unused endpoint survives.
        add("celestron", "telescope", "Telescope", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB4","baudRate":9600,"mountIndex":1,"host":"h"})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB4","baudRate":9600,"mountIndex":1,"host":"h"})");
        add("celestron", "telescope", "Telescope", "network",
            R"({"connectionType":"network","host":"192.168.1.7","tcpPort":2000,"mountIndex":2,"portPath":"/dev/x"})",
            R"({"connectionType":"network","host":"192.168.1.7","tcpPort":2000,"mountIndex":2,"portPath":"/dev/x"})");
        add("celestron", "telescope", "Telescope", "auto", R"({"connectionType":"auto","mountIndex":1})",
            R"({"connectionType":"auto","mountIndex":1})");  // #659
        add("celestron", "telescope", "Telescope", "cgx-aux direct USB",
            R"({"protocol":"cgx-aux","connectionType":"serial","portPath":"/dev/ttyUSB-cgx",)"
            R"("siteLatitude":-33.87,"siteLongitude":151.21,"siteElevation":0.0,)"
            R"("apertureDiameter":0.2,"focalLength":1.0})",
            R"({"protocol":"cgx-aux","connectionType":"serial","portPath":"/dev/ttyUSB-cgx",)"
            R"("siteLatitude":-33.87,"siteLongitude":151.21,"siteElevation":0.0,)"
            R"("apertureDiameter":0.2,"focalLength":1.0})");
        // #860: a known alignmentMode survives; an unknown string or a non-string drops.
        add("celestron", "telescope", "Telescope", "alignmentMode auto",
            R"({"connectionType":"auto","mountIndex":1,"alignmentMode":"auto"})",
            R"({"connectionType":"auto","mountIndex":1,"alignmentMode":"auto"})");
        add("celestron", "telescope", "Telescope", "alignmentMode equatorial",
            R"({"connectionType":"auto","mountIndex":1,"alignmentMode":"equatorial"})",
            R"({"connectionType":"auto","mountIndex":1,"alignmentMode":"equatorial"})");
        add("celestron", "telescope", "Telescope", "alignmentMode non-string",
            R"({"connectionType":"auto","mountIndex":1,"alignmentMode":2})",
            R"({"connectionType":"auto","mountIndex":1})");
        // Focus motor on its own USB port (issue #896): portPath is its only field.
        add("celestron", "focuser", "Focuser", "",
            R"({"portPath":"/dev/ttyUSB-focus","baudRate":9600,"connectionType":"serial"})",
            R"({"portPath":"/dev/ttyUSB-focus"})");
#endif

#ifdef ALPACACORE_ENABLE_BISQUE
        add("bisque", "telescope", "Telescope", "", R"({"host":"skyx.test","tcpPort":3041,"connectionType":"serial"})",
            R"({"host":"skyx.test","tcpPort":3041})");
#endif

#ifdef ALPACACORE_ENABLE_GEMINI
        // The catalog sanitize keeps the declared fields of each type (ADR 0004): the focuser and the
        // switch no longer keep each other's index keys, and a declared portPath/baudRate survives
        // connectionType "auto".
        add("gemini", "focuser", "Focuser", "serial",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB7","baudRate":19200,"focuserIndex":1,"panelIndex":2})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB7","baudRate":19200,"focuserIndex":1})");
        add("gemini", "focuser", "Focuser", "auto", R"({"connectionType":"auto","focuserIndex":1})",
            R"({"connectionType":"auto","focuserIndex":1})");  // #659
        add("gemini", "covercalibrator", "CoverCalibrator", "lite",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB8","baudRate":19200,"panelIndex":2})",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB8","baudRate":19200,"panelIndex":2})");
        add("gemini", "covercalibrator", "CoverCalibrator", "v2",
            R"({"flatPanelModel":"v2","connectionType":"serial","portPath":"/dev/ttyUSB9","baudRate":19200,"panelIndex":3})",
            R"({"flatPanelModel":"v2","connectionType":"serial","portPath":"/dev/ttyUSB9","baudRate":19200,"panelIndex":3})");
        add("gemini", "covercalibrator", "CoverCalibrator", "pro",
            R"({"flatPanelModel":"pro","connectionType":"auto","panelIndex":1,"portPath":"/dev/x","baudRate":9})",
            R"({"flatPanelModel":"pro","connectionType":"auto","panelIndex":1,"portPath":"/dev/x","baudRate":9})");
        add("gemini", "switch", "Switch", "pdh-adv3 auto",
            R"({"switchType":"pdh-adv3","connectionType":"auto","hubIndex":1,"focuserIndex":4})",
            R"({"switchType":"pdh-adv3","connectionType":"auto","hubIndex":1})");
        add("gemini", "switch", "Switch", "pdh-adv3 serial",
            R"({"switchType":"pdh-adv3","connectionType":"serial","portPath":"/dev/ttyUSB3","baudRate":19200,"hubIndex":1})",
            R"({"switchType":"pdh-adv3","connectionType":"serial","portPath":"/dev/ttyUSB3","baudRate":19200,"hubIndex":1})");
#endif

#ifdef ALPACACORE_ENABLE_ALTAIR
        add("altair", "camera", "Camera", "", R"({"cameraIndex":3})", R"({"cameraIndex":3})");
#endif

#ifdef ALPACACORE_ENABLE_ASTROASIS
        add("astroasis", "focuser", "Focuser", "hidPath", R"({"hidPath":"/dev/hidraw3","focuserIndex":2})",
            R"({"hidPath":"/dev/hidraw3","focuserIndex":2})");
        add("astroasis", "focuser", "Focuser", "focuserIndex", R"({"focuserIndex":1})",
            R"({"focuserIndex":1})");  // #659
#endif

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
        // wandererastro covercalibrator and filterwheel had no roundtrip_config() case before #647.
        add("wandererastro", "covercalibrator", "CoverCalibrator", "auto",
            R"({"connectionType":"auto","coverIndex":1,"portPath":"/dev/x"})",
            R"({"connectionType":"auto","coverIndex":1,"portPath":"/dev/x"})");  // declared fields survive (ADR 0004)
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

            auto api = api_attempt(router, c.posted, c.alpaca_type);
            if (!api.ok || api.config != c.expected) {
                std::cerr << "#647 API round trip differs for " << c.label << "\n  error:    " << api.message
                          << "\n  expected: " << c.expected.dump() << "\n  actual:   " << api.config.dump() << "\n";
            }
            EXPECT(api.ok);
            EXPECT(api.config == c.expected);
            remove_device(router, vendor, device_type, number);

            auto persisted = persisted_attempt(c.posted, c.alpaca_type);
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
        // deliberately, when that item's fix lands. (Item 3, an empty
        // connectionType, is resolved: it is "auto" for every mount.)
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
        };
        std::vector<Mount> mounts;
        const char* const kSite = R"("siteLatitude":51.477833,"siteLongitude":-0.0015)";
        const char* const kAutoOrSerialOrNetwork = "Invalid connection type. Use 'auto', 'serial', or 'network'";
#ifdef ALPACACORE_ENABLE_IOPTRON
        mounts.push_back({"ioptron", kAutoOrSerialOrNetwork, ""});
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
        mounts.push_back({"synscan", kAutoOrSerialOrNetwork, ""});
#endif
#ifdef ALPACACORE_ENABLE_SKYWATCHER
        mounts.push_back({"skywatcher", kAutoOrSerialOrNetwork, kSite});
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
        mounts.push_back({"celestron", kAutoOrSerialOrNetwork, ""});
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
        mounts.push_back({"onstep", "Invalid connection type. Use 'auto' or 'serial'", ""});
#endif
#ifdef ALPACACORE_ENABLE_ZWO
        mounts.push_back({"zwo", "Invalid connection type. Use 'serial', 'network', or 'auto'", ""});
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
            // A catalog vendor keeps the port path in the entry too (rule 8 of
            // open-astro#744): sanitize no longer tests == "serial".
            pin("connectionType \"carrier-pigeon\" (#380/#353)", vendor, "telescope", "Telescope",
                obj({R"("connectionType":"carrier-pigeon","portPath":"/dev/ttyUSB9")", site}), m.bad_type_message, "{}",
                true, obj({R"("connectionType":"carrier-pigeon","portPath":"/dev/ttyUSB9")", site}),
                {"has connectionType \"carrier-pigeon\"", warned_serial}, {"Skipping persisted device"});

            // #508 item 2: connectionType is folded to lower case once, on the
            // way into registration and into the stored entry, so "Network"
            // behaves exactly as "network" from the API and from a saved file:
            // registered as a network mount, host kept, no fallback WARN, and
            // configureddevices lists the folded "network". OnStep has no
            // network type, so there the folded value is refused (API) or
            // read as "serial" (saved), naming the folded value.
            if (vendor == "onstep") {
                pin("connectionType \"Network\" (#508 item 2)", vendor, "telescope", "Telescope",
                    obj({R"("connectionType":"Network")", site}), m.bad_type_message, "{}", true,
                    obj({R"("connectionType":"network")", site}), {"has connectionType \"network\"", warned_serial},
                    {"Skipping persisted device"});
            } else {
                const std::string network = obj({R"("connectionType":"network","host":"192.0.2.60")", site});
                pin("connectionType \"Network\" (#508 item 2)", vendor, "telescope", "Telescope",
                    obj({R"("connectionType":"Network","host":"192.0.2.60")", site}), "", network, true, network, {},
                    {warned_serial, "Skipping persisted device"});
            }

            // #508 item 1 (every mount, all catalog descriptors now): an empty
            // portPath on serial is rejected by the API and, from a saved
            // config, WARNED about and registered anyway. Contrast the entries
            // further down that drop the entry.
            // open-astro#744 rule 6: the WARN is the catalog's wrapper
            // ("Persisted <vendor> telescope N config normalized: Serial port
            // path is required. The saved value is not used: ..."), not the
            // deleted reject_invalid_config()'s "will refuse to connect" text.
            pin("serial with empty portPath (#508 item 1, mount arm)", vendor, "telescope", "Telescope",
                obj({R"("connectionType":"serial","portPath":"")", site}), "Serial port path is required", "{}", true,
                obj({R"("connectionType":"serial","portPath":"")", site}),
                std::vector<std::string>{"config normalized: Serial port path is required"},
                std::vector<std::string>{"Skipping persisted device", "will refuse to connect"});

            // #508 item 3: an empty connectionType is "auto" for every mount,
            // zwo included. Its normalize reads "" as auto for both sources, so
            // the API accepts it and a saved one registers with no WARN.
            // zwo only here: the other five arms are pinned by the probe cases.
            if (vendor == "zwo") {  // the other five: probe cases below
                // Both sources list the entry as posted or saved: the router stores the
                // sanitized request (only learned selectors merge back), never the
                // normalized config, so "auto" applies at registration and the listed
                // value stays "" (or absent).
                pin("empty connectionType is auto (#508 item 3)", vendor, "telescope", "Telescope",
                    obj({R"("connectionType":"")", site}), "", obj({R"("connectionType":"")", site}), true,
                    obj({R"("connectionType":"")", site}), {},
                    {"not one this driver knows", warned_serial, "Skipping persisted device"});
                pin("absent connectionType is auto (#508 item 3)", vendor, "telescope", "Telescope", obj({site}), "",
                    "{}", true, "{}", {}, {"not one this driver knows", warned_serial, "Skipping persisted device"});
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
            R"({"connectionType":"network","host":""})", {"config normalized: Host IP address is required"},
            {"Skipping persisted device"});
#endif

#ifdef ALPACACORE_ENABLE_SKYWATCHER
        // #508 item 5: an out-of-range coordinate. The API rejects it; a saved
        // one is WARNED about and ignored by the driver (the #398 test above
        // reads the driver back) and removed from the stored entry, so
        // configureddevices no longer shows 200.0 and the next save does not
        // write it back. The in-range field stays.
        // open-astro#744 rule 7: the range is the catalog's per-field rule, so
        // the API text is the catalog's ("siteLatitude is out of range (min
        // -90) (max 90)"; it was read_site_coordinates()' "siteLatitude
        // 200.000000 is out of range: must be between -90.000000 and 90.000000
        // degrees"). A saved value is dropped to unset with the catalog's
        // wrapped warning, so the factory then logs the missing-site WARNING
        // (#274, connect refusal moved to the pointing members by #1474) as
        // well: two WARNs, the persisted outcome unchanged.
        pin("siteLatitude 200 (#508 item 5)", "skywatcher", "telescope", "Telescope",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB8","siteLatitude":200.0,"siteLongitude":151.215297})",
            "siteLatitude is out of range (min -90) (max 90)", "{}", true,
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB8","siteLongitude":151.215297})",
            {"config normalized: siteLatitude is out of range (min -90) (max 90)", "Persisted Sky-Watcher telescope",
             "has no site latitude; it connects but refuses to point until the site is set"},
            {"Skipping persisted device", "The coordinate is ignored", "siteLatitude 200 is out of range: must be"});
#endif
#ifdef ALPACACORE_ENABLE_BISQUE
        // Bisque moved into the catalog with the same range rule as Sky-Watcher:
        // the API text is the catalog's per-field message, and a saved value is
        // dropped to unset with the wrapped warning, so the listed entry has no
        // siteLatitude, as for Sky-Watcher above. Bisque has no missing-site
        // refusal, so there is no second WARN.
        pin("siteLatitude 200 (catalog range message)", "bisque", "telescope", "Telescope",
            R"({"host":"192.0.2.10","siteLatitude":200.0,"siteLongitude":151.215297})",
            "siteLatitude is out of range (min -90) (max 90)", "{}", true,
            R"({"host":"192.0.2.10","siteLongitude":151.215297})",
            {"config normalized: siteLatitude is out of range (min -90) (max 90)", "Persisted bisque telescope"},
            {"Skipping persisted device", "The coordinate is ignored", "siteLatitude 200 is out of range: must be"});
#endif
#ifdef ALPACACORE_ENABLE_IOPTRON
        // The ioptron schema's normalize words both the API refusal and the saved
        // config's WARN with the text read_site_coordinates() had: short numbers in both.
        pin("siteLatitude 200 (read_site_coordinates)", "ioptron", "telescope", "Telescope",
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB8","siteLatitude":200.0,"siteLongitude":151.215297})",
            "siteLatitude 200 is out of range: must be between -90 and 90 degrees", "{}", true,
            R"({"connectionType":"serial","portPath":"/dev/ttyUSB8","siteLongitude":151.215297})",
            {"config normalized: siteLatitude 200 is out of range: must be between -90 and 90 degrees"},
            {"200.000000"});
#endif

        // #508 item 1, the factories that still DROP a saved entry on an empty portPath
        // instead of registering it with a WARN (the iOptron filterwheel): it is rejected by
        // the API and is NOT registered after a restart. It is still listed, as a
        // "<vendor> (failed to load)" row (LoadError true, lower-case
        // DeviceType), which is how the web UI can show and edit it.
        const std::string kPortRequired = "portPath is required when connectionType is 'serial' (or use 'auto').";
#ifdef ALPACACORE_ENABLE_IOPTRON
        // The catalog turns the refusal into a normalize warning for a saved config, and the
        // factory then throws the same text, so the wheel is dropped as before.
        pin("serial with empty portPath is DROPPED (#508 item 1)", "ioptron", "filterwheel", "FilterWheel",
            R"({"connectionType":"serial","portPath":""})", kPortRequired, "{}", false, "{}",
            {"config normalized: " + kPortRequired}, {});
        // The iGuider opens its devicePath read-write and issues V4L2 ioctls on it, so a
        // path that is not a V4L2 node is refused (decision 0007 rule 6) and dropped the same way.
        const std::string kIguiderPath =
            "devicePath must be empty (auto-detect) or a /dev/video* or /dev/v4l/... capture node.";
        pin("devicePath outside /dev/video* and /dev/v4l/ is DROPPED", "iguider", "camera", "Camera",
            R"({"devicePath":"/dev/sda"})", kIguiderPath, "{}", false, "{}", {"config normalized: " + kIguiderPath},
            {});
        pin("devicePath with a parent step is DROPPED", "iguider", "camera", "Camera",
            R"({"devicePath":"/dev/v4l/../sda"})", kIguiderPath, "{}", false, "{}",
            {"config normalized: " + kIguiderPath}, {});
#endif
#ifdef ALPACACORE_ENABLE_QHY
        // The catalog turns the refusal into a normalize warning for a saved config and the
        // factory builds the wheel anyway, so it stays registered and listed; its connect
        // refuses with the same text (test_qhy_cfw3_filterwheel.cpp). The API still rejects it.
        pin("cfw3-usb serial with empty portPath registers and warns (#508 item 1)", "qhy", "filterwheel",
            "FilterWheel", R"({"wheelType":"cfw3-usb","connectionType":"serial","portPath":""})",
            "QHY CFW3 connectionType \"serial\" requires portPath", "{}", true,
            R"({"wheelType":"cfw3-usb","connectionType":"serial","portPath":""})",
            {"config normalized: QHY CFW3 connectionType \"serial\" requires portPath"}, {"Skipping persisted device"});
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
        // Same shape: a warning, registered and listed, and the connect refuses.
        pin("serial with empty portPath registers and warns (#508 item 1)", "gemini", "switch", "Switch",
            R"({"connectionType":"serial","portPath":""})", kPortRequired, "{}", true,
            R"({"connectionType":"serial","portPath":""})", {"config normalized: " + kPortRequired},
            {"Skipping persisted device"});
#endif
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
        for (const auto& [device_type, alpaca_type] :
             {std::pair<std::string, std::string>{"covercalibrator", "CoverCalibrator"},
              {"rotator", "Rotator"},
              {"filterwheel", "FilterWheel"},
              {"switch", "Switch"}}) {
            pin("serial with empty portPath registers and warns (#508 item 1)", "wandererastro", device_type,
                alpaca_type, R"({"connectionType":"serial","portPath":""})", kPortRequired, "{}", true,
                R"({"connectionType":"serial","portPath":""})", {"config normalized: " + kPortRequired},
                {"Skipping persisted device"});
        }
#endif

        // #508 item 1, the arms that fall through to by-index auto-detect on an
        // empty portPath: registered from both sources, no WARN, and the entry
        // keeps connectionType "serial" with the empty portPath. (Since #659
        // no arm probes hardware while constructing; the mount and focuser
        // auto arms are round-tripped in the #647 table below.)
        const auto silent_pin = [&](const std::string& vendor, const std::string& device_type,
                                    const std::string& alpaca_type, const std::string& extra) {
            const std::string body = obj({R"("connectionType":"serial","portPath":"")", extra});
            pin("serial with empty portPath silently auto-detects (#508 item 1)", vendor, device_type, alpaca_type,
                body, "", body, true, body, {}, {"Serial port path is required", "Skipping persisted device"});
        };
#ifdef ALPACACORE_ENABLE_GEMINI
        silent_pin("gemini", "covercalibrator", "CoverCalibrator", "");
        // #508 item 5 must not widen: a coordinate the descriptor does not declare
        // (sanitize_device_config keeps it for every device type) stays in the entry.
        silent_pin("gemini", "covercalibrator", "CoverCalibrator", R"("siteLatitude":-33.856784)");
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

    // configureddevices carries Connected from get_connected() (web UI status
    // dot); it is omitted when the call throws.
    {
        // case: configureddevices Connected field
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto up = std::make_shared<FirmwareStubDriver>(9511, std::nullopt);
        auto down = std::make_shared<FirmwareStubDriver>(9512, std::nullopt);
        down->connected = false;
        auto broken = std::make_shared<FirmwareStubDriver>(9513, std::nullopt);
        broken->connected_throws = true;
        EXPECT(registry.register_device(up));
        EXPECT(registry.register_device(down));
        EXPECT(registry.register_device(broken));
        auto faulted = std::make_shared<FirmwareStubDriver>(9514, std::nullopt);
        faulted->link_fault = "no status frame for 12 s";
        auto fault_throws = std::make_shared<FirmwareStubDriver>(9515, std::nullopt);
        fault_throws->link_fault_throws = true;
        EXPECT(registry.register_device(faulted));
        EXPECT(registry.register_device(fault_throws));

        const auto json =
            nlohmann::json::parse(route_request(router, "GET", "/management/v1/configureddevices").body());
        int seen = 0;
        for (const auto& entry : json["Value"]) {
            if (entry.value("DeviceType", "") != "CoverCalibrator") {
                continue;
            }
            const int number = entry.value("DeviceNumber", -1);
            if (number == 9511) {
                EXPECT(entry.contains("Connected") && entry["Connected"].get<bool>() == true);
                EXPECT(!entry.contains("LinkFault"));
                ++seen;
            } else if (number == 9512) {
                EXPECT(entry.contains("Connected") && entry["Connected"].get<bool>() == false);
                ++seen;
            } else if (number == 9513) {
                EXPECT(!entry.contains("Connected"));
                ++seen;
            } else if (number == 9514) {
                EXPECT(entry["Connected"].get<bool>() == true);
                EXPECT(entry.contains("LinkFault") &&
                       entry["LinkFault"].get<std::string>() == "no status frame for 12 s");
                ++seen;
            } else if (number == 9515) {
                EXPECT(entry["Connected"].get<bool>() == true);
                EXPECT(!entry.contains("LinkFault"));
                ++seen;
            }
        }
        EXPECT(seen == 5);
    }

    // A device mid-connect is listed Connected=false without get_connected()
    // being called: real drivers hold their coarse mutex across the connect and
    // that call would stall the whole listing (issue #910).
    {
        // case: configureddevices skips get_connected while connecting
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto mid = std::make_shared<FirmwareStubDriver>(9516, std::nullopt);
        mid->connecting = true;
        EXPECT(registry.register_device(mid));
        const auto json =
            nlohmann::json::parse(route_request(router, "GET", "/management/v1/configureddevices").body());
        int seen = 0;
        for (const auto& entry : json["Value"]) {
            if (entry.value("DeviceType", "") == "CoverCalibrator" && entry.value("DeviceNumber", -1) == 9516) {
                EXPECT(entry.contains("Connected") && entry["Connected"].get<bool>() == false);
                ++seen;
            }
        }
        EXPECT(seen == 1);
        EXPECT(mid->get_connected_calls.load() == 0);
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

        // The cap is 64 KiB (issue #741): one byte over fails to parse, and a
        // body of exactly the cap parses in full.
        alpacahttp::Request over_64k_request;
        std::string over_64k =
            "POST /management/v1/configuredevice HTTP/1.1\r\n"
            "Content-Length: 65537\r\n\r\n" +
            std::string(65537, 'x');
        EXPECT(!over_64k_request.parse(over_64k));

        alpacahttp::Request at_64k_request;
        std::string at_64k =
            "POST /management/v1/configuredevice HTTP/1.1\r\n"
            "Content-Length: 65536\r\n\r\n" +
            std::string(65536, 'x');
        EXPECT(at_64k_request.parse(at_64k));
        EXPECT(at_64k_request.body().size() == 65536);
    }

    {
        // case: Telescope DeviceState UTCDate matches the UTCDate endpoint
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto scope = std::make_shared<TelescopeClockStubDriver>(9802);
        EXPECT(registry.register_device(scope));
        alpacahttp::Router clock_router;
        const std::string base = "/api/v1/telescope/9802";
        const auto empty_response = route_request(clock_router, "GET", base + "/devicestate");
        EXPECT(empty_response.status_code() == 200);
        const auto empty_state = nlohmann::json::parse(empty_response.body());
        EXPECT(empty_state.at("ErrorNumber") == 0);
        EXPECT(empty_state.at("Value") == nlohmann::json::array());
        scope->set_connected(true);
        for (const auto ms : {0, 123, 999}) {
            const std::string fraction = ms == 0 ? "000" : ms == 123 ? "123" : "999";
            scope->set_utc_date(std::chrono::system_clock::from_time_t(978307200) + std::chrono::milliseconds{ms});
            const auto utc = nlohmann::json::parse(route_request(clock_router, "GET", base + "/utcdate").body());
            const auto state = nlohmann::json::parse(route_request(clock_router, "GET", base + "/devicestate").body());
            EXPECT(utc.at("ErrorNumber") == 0);
            EXPECT(state.at("ErrorNumber") == 0);
            EXPECT(state.at("Value").is_array());
            int utc_entries = 0;
            for (const auto& entry : state.at("Value")) {
                if (entry.at("Name") == "UTCDate") {
                    ++utc_entries;
                    EXPECT(entry.at("Value").is_string());
                    EXPECT(entry.at("Value") == utc.at("Value"));
                    EXPECT(entry.at("Value") == "2001-01-01T00:00:00." + fraction + "Z");
                }
            }
            EXPECT(utc_entries == 1);
        }
        EXPECT(registry.unregister_device(alpacacore::DeviceType::Telescope, 9802));
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

        // open-astro#670: GET answers through the NowFn seam and refuses a
        // host clock outside the range POST accepts.
        {
            const auto at = [&](std::int64_t secs) {
                router.set_now_fn([secs] { return std::chrono::system_clock::time_point(std::chrono::seconds(secs)); });
                return nlohmann::json::parse(route_request(router, "GET", "/management/v1/synctime").body(), nullptr,
                                             false);
            };
            const auto ok = at(1790467200);  // 2026-09-27T00:00:00Z
            EXPECT(!ok.is_discarded() && ok.value("ErrorNumber", -1) == 0);
            EXPECT(ok["Value"].get<std::int64_t>() == 1790467200);
            for (const std::int64_t bad : {std::int64_t{10}, std::int64_t{4102444801}}) {
                const auto j = at(bad);
                EXPECT(!j.is_discarded());
                EXPECT(j.value("ErrorNumber", 0) == static_cast<int>(alpacacore::AlpacaError::InvalidOperation));
                EXPECT(j.value("ErrorMessage", "") ==
                       "Host clock is outside 2000-01-01..2100-01-01 UTC; set the time with POST "
                       "/management/v1/synctime.");
                EXPECT(!j.contains("Value"));
            }
            for (const std::int64_t edge : {std::int64_t{946684800}, std::int64_t{4102444800}}) {
                const auto j = at(edge);
                EXPECT(j.value("ErrorNumber", -1) == 0);
                EXPECT(j["Value"].get<std::int64_t>() == edge);
            }
            router.set_now_fn([] { return std::chrono::system_clock::now(); });
        }

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
        // request is rejected with 403 before the body is acted on.
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

        // open-astro#674: a ClientTransactionID sent only in the JSON body is
        // echoed on every reply, not only on the cross-origin 403 (#509). A
        // successful POST would set this machine's clock, which a unit test
        // must never do (see above), so the success reply is the GET, served
        // through the NowFn seam; the POST cases are the InvalidValue replies.
        {
            const auto echoed = [](const alpacahttp::Response& response) {
                const auto json = nlohmann::json::parse(response.body(), nullptr, false);
                EXPECT(!json.is_discarded());
                return json.value("ClientTransactionID", 0U);
            };
            router.set_now_fn([] { return std::chrono::system_clock::time_point(std::chrono::seconds(1790467200)); });
            const auto ok = route_request(router, "GET", "/management/v1/synctime", R"({"ClientTransactionID": 4242})");
            EXPECT(nlohmann::json::parse(ok.body(), nullptr, false).value("ErrorNumber", -1) == 0);
            EXPECT(echoed(ok) == 4242U);
            router.set_now_fn([] { return std::chrono::system_clock::now(); });

            for (const char* body :
                 {R"({"Epoch": 100, "ClientTransactionID": 4242})", R"({"ClientTransactionID": 4242})"}) {
                const auto response = route_request(router, "POST", "/management/v1/synctime", body);
                const auto json = nlohmann::json::parse(response.body(), nullptr, false);
                EXPECT(!json.is_discarded());
                EXPECT(json.value("ErrorNumber", 0) == static_cast<int>(alpacacore::AlpacaError::InvalidValue));
                EXPECT(echoed(response) == 4242U);
            }

            // Same precedence as the 403 path: a non-zero query-string ID wins.
            const auto both = route_request(router, "POST", "/management/v1/synctime?ClientTransactionID=7",
                                            R"({"Epoch": 100, "ClientTransactionID": 4242})");
            EXPECT(echoed(both) == 7U);
        }
    }

    // open-astro#675: set_now_fn() may replace the clock while a request
    // thread is inside the previous one. The call in flight must keep its
    // callable alive until it returns, the shape HostClock::set_hooks() has
    // (#399); a plain std::function assignment destroys it under the reader.
    {
        alpacahttp::Router router;
        // Function-local statics, not captures: on the unfixed code the
        // callable is destroyed while it runs, and a body that reached its
        // own captures after that would read freed memory before the check
        // below could report it.
        static std::mutex gate_mutex;
        static std::condition_variable gate_cv;
        static bool entered = false;
        static bool release = false;
        struct Token {
            std::shared_ptr<std::atomic<bool>> destroyed;
            ~Token() { destroyed->store(true); }
        };
        const auto destroyed = std::make_shared<std::atomic<bool>>(false);
        auto token = std::make_shared<Token>();
        token->destroyed = destroyed;
        router.set_now_fn([token] {
            std::unique_lock<std::mutex> lock(gate_mutex);
            entered = true;
            gate_cv.notify_all();
            gate_cv.wait(lock, [] { return release; });
            return std::chrono::system_clock::time_point(std::chrono::seconds(1790467200));
        });
        token.reset();

        nlohmann::json reply;
        std::thread reader([&] {
            reply =
                nlohmann::json::parse(route_request(router, "GET", "/management/v1/synctime").body(), nullptr, false);
        });
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            EXPECT(gate_cv.wait_for(lock, std::chrono::seconds(30), [] { return entered; }));
        }
        router.set_now_fn([] { return std::chrono::system_clock::now(); });
        const bool alive_after_swap = !destroyed->load();
        {
            std::lock_guard<std::mutex> lock(gate_mutex);
            release = true;
        }
        gate_cv.notify_all();
        reader.join();
        EXPECT(alive_after_swap);
        // The in-flight call finished on the callable it started with...
        EXPECT(reply.value("ErrorNumber", -1) == 0);
        EXPECT(reply.value("Value", std::int64_t{0}) == 1790467200);
        // ...which was released once it returned, not leaked...
        EXPECT(destroyed->load());
        // ...and the next request uses the replacement.
        const auto next =
            nlohmann::json::parse(route_request(router, "GET", "/management/v1/synctime").body(), nullptr, false);
        EXPECT(next.value("Value", std::int64_t{0}) > 1600000000 && next.value("Value", std::int64_t{0}) != 1790467200);
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
                 std::pair{"/management/v1/wifi/profiles", "{\"SsidHex\": \"gg\"}"},
                 std::pair{"/management/v1/wifi/connect", "not json"},
                 std::pair{"/management/v1/wifi/ap", "{\"Ssid\": \"x\", \"Band\": \"g\"}"},
                 std::pair{"/management/v1/wifi/ap", "{\"SsidHex\": \"f\"}"},
                 std::pair{"/management/v1/wifi/radio", "{\"Enabled\": \"yes\"}"},
             }) {
            const auto response = route_request(router, "PUT", path, body);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        }
    }

    {
        // case: a Wi-Fi integer outside its field's type is refused by name, never wrapped (#1476, ADR 0007)
        // get<int>() wrapped Priority 4294967296 to 0 and get<uint32_t>() Channel
        // 4294967297 to 1. A wrapped call still fails later in NetworkManager,
        // so the field name in ErrorMessage is the signal.
        alpacahttp::Router router;
        for (const auto& [path, body, field] : {
                 std::tuple{"/management/v1/wifi/profiles", "{\"Ssid\":\"x\",\"Priority\":4294967296}", "Priority"},
                 std::tuple{"/management/v1/wifi/profiles", "{\"Ssid\":\"x\",\"Priority\":18446744073709551615}",
                            "Priority"},
                 std::tuple{"/management/v1/wifi/ap", "{\"Ssid\":\"x\",\"Channel\":4294967297}", "Channel"},
             }) {
            const auto response = route_request(router, "PUT", path, body);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            const auto message = json.is_discarded() ? std::string{} : json.value("ErrorMessage", std::string{});
            if (message.find(field) == std::string::npos) {
                std::cerr << path << " " << body << " -> " << response.body() << "\n";
            }
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
            EXPECT(message.find(field) != std::string::npos);
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
    // connect (async_connectable.h's blocking list is empty today; no real
    // driver has this shape). SynScan produced #130 and its getter has been a
    // bare atomic load since that fix. The router must poll
    // get_connecting(), the non-blocking signal, so GET connected/connecting
    // answer at once mid-connect and the PUT connected wait honours its
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
        // on the driver mutex for the entire connect, so the wait deadline
        // never fired. Issue #776: PUT Connected=true now returns only once
        // the connect has finished, so a 9.5 s handshake (past the old 8 s
        // reply) answers success with the link already up and Connecting
        // false. (Deliberately ~10 s of wall clock: the old 8 s reply is the
        // thing under test.)
        auto slow = std::make_shared<LockedSlowConnectStubDriver>(9703, Ms(9500));
        EXPECT(registry.register_device(slow));
        const std::string slow_base = "/api/v1/covercalibrator/9703";
        const auto put_started = std::chrono::steady_clock::now();
        put_connected(router, slow_base, "1", true);
        EXPECT(elapsed_ms(put_started) >= 9500);
        EXPECT(!slow->get_connecting());
        EXPECT(slow->get_connected());
        EXPECT(get_connected_value(router, slow_base, "1"));
        put_connected(router, slow_base, "1", false);
        EXPECT(!slow->get_connected());

        // The wait still has a bound, and #130 still matters for it: with a
        // 1 s limit and a 4 s handshake that holds the driver mutex, the reply
        // comes back near the limit (not after the handshake) as a
        // DriverException, and the abandoned connect is withdrawn so the
        // device does not come up behind a client that was told it failed.
        alpacahttp::Router wait_router;
        wait_router.set_connect_wait_limit(Ms(1000));
        auto capped = std::make_shared<LockedSlowConnectStubDriver>(9704, Ms(4000));
        EXPECT(registry.register_device(capped));
        const std::string capped_base = "/api/v1/covercalibrator/9704";
        const auto capped_started = std::chrono::steady_clock::now();
        {
            const auto resp =
                route_request(wait_router, "PUT", capped_base + "/connected", "Connected=true&ClientID=1");
            const auto json = nlohmann::json::parse(resp.body(), nullptr, false);
            EXPECT(!json.is_discarded());
            EXPECT(json.value("ErrorNumber", 0) == static_cast<int>(alpacacore::AlpacaError::DriverException));
            EXPECT(json.value("ErrorMessage", "").find("still connecting") != std::string::npos);
        }
        EXPECT(elapsed_ms(capped_started) < 2500);
        EXPECT(!get_connected_value(wait_router, capped_base, "1"));
        for (int i = 0; i < 100 && capped->get_connecting(); ++i) {
            std::this_thread::sleep_for(Ms(100));
        }
        EXPECT(!capped->get_connecting());
        EXPECT(!capped->get_connected());
        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9704);
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
        // Issue #776: a DriverException. ASCOM Connected says "Do not use a
        // NotConnectedException here": the device refused, it was not asked
        // to work while disconnected.
        EXPECT(json.value("ErrorNumber", 0) == static_cast<int>(alpacacore::AlpacaError::DriverException));
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

        // Issue #776: the completion property reports the failure. GET
        // Connecting raises the driver's reason as a DriverException, on every
        // read, until the client's next Connect or Disconnect.
        const auto connecting_reply = [&router]() {
            return nlohmann::json::parse(route_request(router, "GET", "/api/v1/covercalibrator/9650/connecting").body(),
                                         nullptr, false);
        };
        for (int read = 0; read < 2; ++read) {
            const auto failed = connecting_reply();
            EXPECT(!failed.is_discarded());
            EXPECT(failed.value("ErrorNumber", 0) == static_cast<int>(alpacacore::AlpacaError::DriverException));
            EXPECT(failed.value("ErrorMessage", "") == std::string(RefusingConnectStubDriver::kReason));
        }

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

        // Disconnect resets the Connecting error; LastConnectError stays.
        EXPECT(nlohmann::json::parse(route_request(router, "PUT", "/api/v1/covercalibrator/9650/disconnect", "").body(),
                                     nullptr, false)
                   .value("ErrorNumber", -1) == 0);
        for (int i = 0; i < 100 && refusing->get_connecting(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        {
            const auto cleared = connecting_reply();
            EXPECT(!cleared.is_discarded() && cleared.value("ErrorNumber", -1) == 0);
            EXPECT(cleared.contains("Value") && cleared["Value"] == false);
        }
        EXPECT(refusing->get_last_connect_error() == std::string(RefusingConnectStubDriver::kReason));

        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9650);
    }

    // Issue #776: a synchronous connect() that throws NotConnected is reported
    // as a DriverException, and the failed client's registration is dropped.
    {
        // case: sync connect failure
        alpacahttp::Router router;
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto stub = std::make_shared<ThrowingSyncConnectStubDriver>(9651);
        EXPECT(registry.register_device(stub));

        const auto failed = nlohmann::json::parse(
            route_request(router, "PUT", "/api/v1/covercalibrator/9651/connected", "Connected=true&ClientID=1").body(),
            nullptr, false);
        EXPECT(!failed.is_discarded());
        EXPECT(failed.value("ErrorNumber", 0) == static_cast<int>(alpacacore::AlpacaError::DriverException));
        EXPECT(failed.value("ErrorMessage", "") == std::string(ThrowingSyncConnectStubDriver::kReason));

        // Client 1 holds no registration, so another client's disconnect is
        // the last one out and reaches the driver.
        const auto released = nlohmann::json::parse(
            route_request(router, "PUT", "/api/v1/covercalibrator/9651/connected", "Connected=false&ClientID=2").body(),
            nullptr, false);
        EXPECT(!released.is_discarded() && released.value("ErrorNumber", -1) == 0);
        EXPECT(stub->disconnect_calls == 1);

        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, 9651);
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

    // Issue #509: the 403 also echoes a ClientTransactionID that arrives only
    // in the JSON body.
    {
        alpacahttp::Router router;
        for (const char* path : {"/management/v1/description", "/management/v1/loglevel", "/management/v1/synctime"}) {
            for (const char* method : {"PUT", "POST"}) {
                const std::string body = R"({"ClientTransactionID": 4242})";
                std::ostringstream raw;
                raw << method << " " << path << " HTTP/1.1\r\n"
                    << "Host: localhost\r\n"
                    << "Origin: http://evil.example\r\n"
                    << "Content-Type: application/json\r\n"
                    << "Content-Length: " << body.size() << "\r\n\r\n"
                    << body;
                alpacahttp::Request request;
                EXPECT(request.parse(raw.str()));
                const auto response = router.route(request, 1);
                EXPECT(response.status_code() == 403);
                const auto json = nlohmann::json::parse(response.body(), nullptr, false);
                EXPECT(!json.is_discarded());
                EXPECT(json.value("ClientTransactionID", 0U) == 4242U);
                EXPECT(json.value("ErrorMessage", "").find("Cross-origin") != std::string::npos);
            }
        }
    }

    {
        // case: a deviceNumber outside int or with a fraction is refused by name, logged below WARNING (#1476)
        // ADR 0007 rule 4: get<int>() wrapped 4294967299 to device 3 and cut
        // 3.5 to 3. Rule 9: a config error the client sent is not logged at
        // WARNING or above.
        std::vector<std::string> loud;
        std::mutex loud_mutex;
        struct SinkRestore {
            alpacacore::logging::LogSink sink = alpacacore::logging::get_log_sink();
            ~SinkRestore() { alpacacore::logging::set_log_sink(sink); }
        } sink_restore;
        alpacacore::logging::set_log_sink(
            [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                if (level < alpacacore::logging::LogLevel::Warn) return;
                std::lock_guard<std::mutex> lock(loud_mutex);
                loud.emplace_back(message);
            });

        alpacahttp::Router router;
        for (const auto& [path, body, field] : {
                 std::tuple{"/management/v1/configuredevice",
                            R"({"vendor":"nosuchvendor","deviceType":"focuser","deviceNumber":4294967299})",
                            "deviceNumber"},
                 std::tuple{"/management/v1/configuredevice",
                            R"({"vendor":"nosuchvendor","deviceType":"focuser","deviceNumber":3.5})", "deviceNumber"},
                 std::tuple{"/management/v1/removedevice",
                            R"({"vendor":"nosuchvendor","deviceType":"focuser","deviceNumber":4294967299})",
                            "deviceNumber"},
                 std::tuple{"/management/v1/configuredevice",
                            R"({"vendor":"nosuchvendor","deviceType":"focuser","deviceNumber":true})",
                            "'deviceNumber' has the wrong type (got boolean)"},
                 std::tuple{"/management/v1/removedevice",
                            R"({"vendor":"nosuchvendor","deviceType":"focuser","deviceNumber":true})",
                            "'deviceNumber' has the wrong type (got boolean)"},
                 std::tuple{"/management/v1/configuredevice",
                            R"({"vendor":"nosuchvendor","deviceType":5,"deviceNumber":1})", "deviceType"},
                 std::tuple{"/management/v1/removedevice",
                            R"({"vendor":"nosuchvendor","deviceType":5,"deviceNumber":1})", "deviceType"},
             }) {
            const auto response = route_request(router, "POST", path, body);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            const auto message = json.is_discarded() ? std::string{} : json.value("ErrorMessage", std::string{});
            if (message.find(field) == std::string::npos) {
                std::cerr << path << " " << body << " -> " << response.body() << "\n";
            }
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
            EXPECT(message.find(field) != std::string::npos);
        }
        {
            std::lock_guard<std::mutex> lock(loud_mutex);
            for (const auto& line : loud) std::cerr << "logged at WARNING or above: " << line << "\n";
            EXPECT(loud.empty());
        }
    }

    {
        // case: a persisted deviceNumber outside int is listed as failed with no number (#1476)
        // It has no key, so it does not stand in for device 3, the number
        // get<int>() wrapped it to.
        const ScopedCwd scratch;
        std::filesystem::create_directories("config");
        {
            std::ofstream out(std::filesystem::path("config") / "registered_devices.json", std::ios::trunc);
            out << R"([{"vendor":"nosuchvendor","deviceType":"focuser","deviceNumber":4294967299}])";
        }
        alpacahttp::Router router;
        const auto listed =
            nlohmann::json::parse(route_request(router, "GET", "/management/v1/faileddevices").body(), nullptr, false);
        EXPECT(!listed.is_discarded() && listed.contains("Value") && listed["Value"].is_array());
        EXPECT(listed["Value"].size() == 1);
        if (listed["Value"][0].value("DeviceNumber", 0) != -1) std::cerr << listed.dump() << "\n";
        EXPECT(listed["Value"][0].value("DeviceNumber", 0) == -1);
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
        // config_from_json() (the catalog bridge) reports it as an AlpacaException naming
        // the field rather than letting nlohmann's type_error reach the outer catch.
        //
        // The null case above does NOT cover this: it asserts only the absence
        // of "json.exception" text, and passes with or without the try/catch,
        // because a null never reaches get<T>() at all. Deleting the wrong-type
        // check in config_from_json() (the Double arm) must fail HERE.
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

    // open-astro#744: an Int field takes an integer or a whole-number float
    // (9600 or 9600.0); a fractional value is a wrong-type refusal naming the
    // field, as for every catalog vendor (catalog_json.cpp whole_number()).
    // The deleted arm's get<int>() read 9600.5 as 9600 without a word. The
    // catalog rule wins; the PR body quotes the change.
    {
        alpacahttp::Router router;
        const auto fractional = api_attempt(
            router,
            nlohmann::json::parse(R"({"vendor":"skywatcher","deviceType":"telescope","deviceNumber":9259,)"
                                  R"("connectionType":"serial","portPath":"/dev/ttyUSB6","baudRate":9600.5,)"
                                  R"("siteLatitude":51.477833,"siteLongitude":-0.0015})"),
            "Telescope");
        EXPECT(!fractional.ok);
        EXPECT(fractional.message.find("baudRate") != std::string::npos);
        EXPECT(fractional.message == "Device config field 'baudRate' must be a whole number (got 9600.5)");
        EXPECT(listed_entry(router, "Telescope", 9259).is_null());

        const auto boolean =
            api_attempt(router,
                        nlohmann::json::parse(R"({"vendor":"skywatcher","deviceType":"telescope","deviceNumber":9259,)"
                                              R"("connectionType":"serial","portPath":"/dev/ttyUSB6","baudRate":true,)"
                                              R"("siteLatitude":51.477833,"siteLongitude":-0.0015})"),
                        "Telescope");
        EXPECT(!boolean.ok);
        EXPECT(boolean.message == "Device config field 'baudRate' must be a whole number (got true)");

        const auto text = api_attempt(
            router,
            nlohmann::json::parse(R"({"vendor":"skywatcher","deviceType":"telescope","deviceNumber":9259,)"
                                  R"("connectionType":"serial","portPath":"/dev/ttyUSB6","baudRate":"9600",)"
                                  R"("siteLatitude":51.477833,"siteLongitude":-0.0015})"),
            "Telescope");
        EXPECT(!text.ok);
        EXPECT(text.message == "Device config field 'baudRate' has the wrong type (got string)");

        const auto whole = api_attempt(
            router,
            nlohmann::json::parse(R"({"vendor":"skywatcher","deviceType":"telescope","deviceNumber":9259,)"
                                  R"("connectionType":"serial","portPath":"/dev/ttyUSB6","baudRate":9600.0,)"
                                  R"("siteLatitude":51.477833,"siteLongitude":-0.0015})"),
            "Telescope");
        EXPECT(whole.ok);
        EXPECT(whole.config.value("baudRate", -1) == 9600);
        remove_device(router, "skywatcher", "telescope", 9259);
    }
#endif  // ALPACACORE_ENABLE_SKYWATCHER

#ifdef ALPACACORE_ENABLE_IOPTRON
    // The iOptron schema's normalize words the refusal with the text the router's
    // read_site_coordinates() had: the numbers print in their short form, not std::to_string's
    // "200.000000" / "-90.000000".
    {
        alpacahttp::Router router;
        for (const auto& [override_json, expected] :
             {std::pair{nlohmann::json{{"siteLatitude", 200.0}},
                        std::string("siteLatitude 200 is out of range: must be between -90 and 90 degrees")},
              std::pair{nlohmann::json{{"siteLongitude", 999.5}},
                        std::string("siteLongitude 999.5 is out of range: must be between -180 and 180 degrees")}}) {
            nlohmann::json config = {{"vendor", "ioptron"},
                                     {"deviceType", "telescope"},
                                     {"deviceNumber", 9641},
                                     {"connectionType", "serial"},
                                     {"portPath", "/dev/null"}};
            config.update(override_json);
            const auto response = route_request(router, "POST", "/management/v1/configuredevice", config.dump());
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
            EXPECT(json.value("ErrorMessage", "").find(expected) != std::string::npos);
        }
    }
#endif

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
            // Software update (docs/software-update.md): check fetches from
            // the network, install starts the root helper unit.
            {"POST", "/management/v1/update/check", "{}", "software update"},
            {"POST", "/management/v1/update/install", "{}", "software update"},
            {"POST", "/management/v1/update/channel", R"({"Channel":"beta"})", "software update"},
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

    // Software update endpoints (docs/software-update.md) over a scripted
    // backend: the route, the envelope, the not-configured answer, and that
    // the router maps the manager's refusals onto their Alpaca codes.
    {
        // Not configured (no manager installed): every sub-endpoint answers
        // NOT_IMPLEMENTED rather than 404, like restart without a callback.
        alpacahttp::Router bare;
        const auto bare_json =
            nlohmann::json::parse(route_request(bare, "GET", "/management/v1/update/status").body(), nullptr, false);
        EXPECT(!bare_json.is_discarded());
        EXPECT(bare_json.value("ErrorNumber", 0) == alpacahttp::util::ErrorCode::NOT_IMPLEMENTED);
        EXPECT(bare_json.value("ErrorMessage", "").find("not configured") != std::string::npos);

        class ScriptedBackend final : public alpacahttp::util::SoftwareUpdateBackend {
        public:
            std::string index = "Package: alpacabridge\nVersion: 99.0.0\n";
            int starts = 0;
            alpacahttp::util::InstallerState state;
            std::string fetch_url(const std::string& url, std::chrono::milliseconds) override {
                if (url.find("notes.example") != std::string::npos) return "# AlpacaBridge 99.0.0\n\n- New.\n";
                return index;
            }
            void start_installer() override {
                ++starts;
                state.state = "running";
            }
            alpacahttp::util::InstallerState installer_state() override { return state; }
            alpacahttp::util::UpdateChannel channel = alpacahttp::util::UpdateChannel::Stable;
            std::vector<alpacahttp::util::UpdateChannel> switches;
            alpacahttp::util::InstallerState channel_state;
            alpacahttp::util::UpdateChannel read_channel() override { return channel; }
            void start_channel_switch(alpacahttp::util::UpdateChannel requested) override {
                switches.push_back(requested);
                channel_state.state = "running";
            }
            alpacahttp::util::InstallerState channel_switch_state() override { return channel_state; }
        };

        alpacahttp::Router router;
        auto backend = std::make_unique<ScriptedBackend>();
        auto* scripted = backend.get();
        router.set_software_update_manager(std::make_unique<alpacahttp::util::SoftwareUpdateManager>(
            alpacahttp::util::SoftwareUpdateSettings{alpacahttp::kVersion, "https://apt.example/Packages",
                                                     "alpacabridge", "https://notes.example/{version}.md",
                                                     "https://rel.example/v{version}"},
            std::move(backend)));

        // GET status before any check: the running version, nothing else.
        auto json = nlohmann::json::parse(
            route_request(router, "GET", "/management/v1/update/status?ClientTransactionID=4242").body(), nullptr,
            false);
        EXPECT(!json.is_discarded());
        EXPECT(json.value("ErrorNumber", -1) == 0);
        EXPECT(json.value("ClientTransactionID", 0U) == 4242U);
        EXPECT(json["Value"]["InstalledVersion"] == std::string(alpacahttp::kVersion));
        EXPECT(json["Value"]["LatestVersion"].is_null());
        EXPECT(json["Value"]["UpdateAvailable"] == false);
        EXPECT(json["Value"]["Installer"]["State"] == "idle");

        // Install before a check is INVALID_OPERATION and never reaches the backend.
        json = nlohmann::json::parse(route_request(router, "POST", "/management/v1/update/install", "{}").body(),
                                     nullptr, false);
        EXPECT(json.value("ErrorNumber", 0) == alpacahttp::util::ErrorCode::INVALID_OPERATION);
        EXPECT(scripted->starts == 0);

        // The unversioned alias and PUT are accepted for check.
        json = nlohmann::json::parse(route_request(router, "PUT", "/management/update/check", "{}").body(), nullptr,
                                     false);
        EXPECT(json.value("ErrorNumber", -1) == 0);
        EXPECT(json["Value"]["LatestVersion"] == "99.0.0");
        EXPECT(json["Value"]["UpdateAvailable"] == true);
        EXPECT(json["Value"]["CheckedAt"].is_number_integer());
        EXPECT(json["Value"]["CheckError"].is_null());
        // The newer version's notes and links ride along for the card.
        EXPECT(json["Value"]["ReleaseNotes"] == "# AlpacaBridge 99.0.0\n\n- New.\n");
        EXPECT(json["Value"]["ReleaseNotesUrl"] == "https://notes.example/99.0.0.md");
        EXPECT(json["Value"]["ReleaseUrl"] == "https://rel.example/v99.0.0");

        // Install now starts the helper exactly once and reports it running.
        json = nlohmann::json::parse(route_request(router, "POST", "/management/v1/update/install", "{}").body(),
                                     nullptr, false);
        EXPECT(json.value("ErrorNumber", -1) == 0);
        EXPECT(scripted->starts == 1);
        EXPECT(json["Value"]["Installer"]["State"] == "running");

        // Wrong verb or unknown sub-endpoint: INVALID_VALUE, still HTTP 200.
        const auto wrong_verb = route_request(router, "GET", "/management/v1/update/install");
        EXPECT(wrong_verb.status_code() == 200);
        json = nlohmann::json::parse(wrong_verb.body(), nullptr, false);
        EXPECT(json.value("ErrorNumber", 0) == alpacahttp::util::ErrorCode::INVALID_VALUE);
        json = nlohmann::json::parse(route_request(router, "POST", "/management/v1/update/status", "{}").body(),
                                     nullptr, false);
        EXPECT(json.value("ErrorNumber", 0) == alpacahttp::util::ErrorCode::INVALID_VALUE);
        json =
            nlohmann::json::parse(route_request(router, "GET", "/management/v1/update/bogus").body(), nullptr, false);
        EXPECT(json.value("ErrorNumber", 0) == alpacahttp::util::ErrorCode::INVALID_VALUE);
        {
            // case: update channel route
            // Channel switch (issue #1103): body {"Channel":"stable"|"beta"}.
            // The stock stable index follows the channel; a configured URL would pin it.
            alpacahttp::Router router;
            auto channel_backend = std::make_unique<ScriptedBackend>();
            auto* scripted = channel_backend.get();
            router.set_software_update_manager(std::make_unique<alpacahttp::util::SoftwareUpdateManager>(
                alpacahttp::util::SoftwareUpdateSettings{alpacahttp::kVersion, alpacahttp::util::kDefaultPackagesUrl,
                                                         "alpacabridge", "", ""},
                std::move(channel_backend)));
            auto channel_json = [&](const std::string& method, const std::string& body) {
                const auto response = route_request(router, method, "/management/v1/update/channel", body);
                return std::make_pair(response.status_code(), nlohmann::json::parse(response.body(), nullptr, false));
            };
            for (const char* bad : {"{}", "", "not json", R"({"Channel":"Beta"})", R"({"Channel":"beta;x"})",
                                    R"({"Channel":1})", R"({"Channel":null})", R"({"Channel":"../stable"})"}) {
                const auto [status, reply] = channel_json("POST", bad);
                EXPECT(status == 400);
                EXPECT(!reply.is_discarded());
                EXPECT(reply.value("ErrorNumber", 0) == alpacahttp::util::ErrorCode::INVALID_VALUE);
                // The rejected text is not echoed back.
                EXPECT(reply.value("ErrorMessage", "").find("beta;x") == std::string::npos);
            }
            EXPECT(scripted->switches.empty());
            // An AlpacaBridge-only request: its 400 keeps the JSON envelope (#1476, ADR 0007).
            const auto refused = route_request(router, "PUT", "/management/v1/update/channel", R"({"Channel":"nope"})");
            EXPECT(refused.status_code() == 400);
            EXPECT(refused.get_header("Content-Type").find("application/json") == 0);
            EXPECT(route_request(router, "GET", "/management/v1/update/channel").status_code() == 200);
            EXPECT(scripted->switches.empty());
            {
                const auto [status, reply] = channel_json("POST", R"({"Channel":"beta"})");
                EXPECT(status == 200);
                EXPECT(reply.value("ErrorNumber", -1) == 0);
                EXPECT(scripted->switches.size() == 1);
                EXPECT(scripted->switches[0] == alpacahttp::util::UpdateChannel::Beta);
                EXPECT(reply["Value"]["Channel"] == "stable");  // the marker has not changed yet
                EXPECT(reply["Value"]["ChannelSwitch"]["State"] == "running");
                // Busy: refused with the install route's INVALID_OPERATION, 200 envelope.
                const auto [busy_status, busy] = channel_json("PUT", R"({"Channel":"stable"})");
                EXPECT(busy_status == 200);
                EXPECT(busy.value("ErrorNumber", 0) == alpacahttp::util::ErrorCode::INVALID_OPERATION);
                EXPECT(scripted->switches.size() == 1);
            }
            scripted->channel = alpacahttp::util::UpdateChannel::Beta;
            scripted->channel_state.state = "succeeded";
            json = nlohmann::json::parse(route_request(router, "GET", "/management/v1/update/status").body(), nullptr,
                                         false);
            EXPECT(json["Value"]["Channel"] == "beta");
            EXPECT(json["Value"]["ChannelOverridden"] == false);
            EXPECT(json["Value"]["ChannelSwitch"]["State"] == "succeeded");
        }

        // A path outside the sub-endpoint grammar is not a management route.
        EXPECT(route_request(router, "GET", "/management/v1/update/status/extra").status_code() == 404);
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

    // open-astro#664 Part B: GET /management/v1/devicecatalog serves the
    // catalog in the management envelope. The shape is pinned by the committed
    // fixture tests/fixtures/devicecatalog.json (a fixture change is a
    // deliberate commit). The catalog under test holds the built-in Altair, Astroasis
    // and the Bisque, Celestron, Gemini, gphoto, OnStep, Player One, SkyWatcher (open-astro#744), QHY, SVBONY, SynScan,
    // ToupTek, WandererAstro, WeeWX and ZWO descriptors plus the "zzz" test descriptor, schema only, so
    // its `available` is false.
    {
        alpacahttp::Router router;
        alpacahttp::test_catalog::add_schema(router.catalog());

        const std::filesystem::path fixture_path = std::filesystem::path(ALPACAHTTP_ROUTER_SRC_DIR).parent_path() /
                                                   "tests" / "fixtures" / "devicecatalog.json";
        std::ifstream fixture_in(fixture_path);
        EXPECT(fixture_in.good());
        nlohmann::json fixture = nlohmann::json::parse(fixture_in, nullptr, false);
        EXPECT(!fixture.is_discarded() && fixture.is_array() && fixture.size() == 41);
        // The fixture is written for the all-vendors build. `available` is the
        // one value that depends on the build (true with the vendor on, false
        // with ALPACACORE_ENABLE_<VENDOR>=OFF), so it is set from this build
        // before the compare; every other byte must match.
        for (auto& entry : fixture) {
            if (entry.value("vendor", "") == "altair") {
#ifdef ALPACACORE_ENABLE_ALTAIR
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "astroasis") {
#ifdef ALPACACORE_ENABLE_ASTROASIS
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "bisque") {
#ifdef ALPACACORE_ENABLE_BISQUE
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "gphoto") {
#ifdef ALPACACORE_ENABLE_GPHOTO
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "celestron") {
#ifdef ALPACACORE_ENABLE_CELESTRON
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "qhy") {
#ifdef ALPACACORE_ENABLE_QHY
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "synscan") {
#ifdef ALPACACORE_ENABLE_SYNSCAN
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "onstep") {
#ifdef ALPACACORE_ENABLE_ONSTEP
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "playerone") {
#ifdef ALPACACORE_ENABLE_PLAYERONE
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "skywatcher") {
#ifdef ALPACACORE_ENABLE_SKYWATCHER
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "svbony") {
#ifdef ALPACACORE_ENABLE_SVBONY
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "iguider") {
                // The iOptron iGuider guide camera: an iOptron driver under its own vendor key.
#ifdef ALPACACORE_ENABLE_IOPTRON
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "ioptron") {
                // The iCAM camera is built by the Player One option; the other four follow the
                // iOptron option (the PowerBox switch is registered without libgpiod too and
                // refuses with its "not built" text).
                const std::string type = entry.value("deviceType", "");
                if (type == "camera") {
#ifdef ALPACACORE_ENABLE_PLAYERONE
                    entry["available"] = true;
#else
                    entry["available"] = false;
#endif
                } else {
#ifdef ALPACACORE_ENABLE_IOPTRON
                    entry["available"] = true;
#else
                    entry["available"] = false;
#endif
                }
            }
            if (entry.value("vendor", "") == "gemini") {
#ifdef ALPACACORE_ENABLE_GEMINI
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "wandererastro") {
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "zwo") {
#ifdef ALPACACORE_ENABLE_ZWO
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "touptek") {
#ifdef ALPACACORE_ENABLE_TOUPTEK
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
            if (entry.value("vendor", "") == "weewx") {
#ifdef ALPACACORE_ENABLE_WEEWX
                entry["available"] = true;
#else
                entry["available"] = false;
#endif
            }
        }

        const auto response = route_request(router, "GET", "/management/v1/devicecatalog?ClientTransactionID=4242");
        EXPECT(response.status_code() == 200);
        const auto json = nlohmann::json::parse(response.body(), nullptr, false);
        EXPECT(!json.is_discarded());
        EXPECT(json.value("ErrorNumber", -1) == 0);
        EXPECT(json.value("ErrorMessage", "x").empty());
        EXPECT(json.value("ClientTransactionID", 0) == 4242);
        EXPECT(json.contains("ServerTransactionID") && json["ServerTransactionID"].is_number_unsigned());
        EXPECT(json.contains("Value") && json["Value"].is_array());
        // Canonical compare: dump() of the parsed objects on both sides.
        EXPECT(json["Value"].dump() == fixture.dump());

        // The unversioned alias resolves to the same handler.
        const auto alias =
            nlohmann::json::parse(route_request(router, "GET", "/management/devicecatalog").body(), nullptr, false);
        EXPECT(!alias.is_discarded() && alias.value("ErrorNumber", -1) == 0);
        EXPECT(alias["Value"].dump() == fixture.dump());

        // GET only: a PUT or POST is refused the way the description endpoint
        // refuses one -- 405 with InvalidOperation (0x40B) in the envelope.
        const auto put = route_request(router, "PUT", "/management/v1/devicecatalog", "ClientTransactionID=77");
        EXPECT(put.status_code() == 405);
        EXPECT(put.get_header("Allow") == "GET");
        const auto put_json = nlohmann::json::parse(put.body(), nullptr, false);
        EXPECT(!put_json.is_discarded() && put_json.value("ErrorNumber", 0) == 0x40B);
        const auto post = route_request(router, "POST", "/management/v1/devicecatalog", "{}");
        EXPECT(post.status_code() == 405);
        EXPECT(post.get_header("Allow") == "GET");
    }

    // GET /management/v1/thermalcameras?vendor=<v> serves the catalog's camera
    // lister for a thermal switch's camera picker, in the management envelope.
    {
        // case: thermalcameras serves the vendor's camera listing from the catalog
        alpacahttp::Router router;
        int lister_calls = 0;
        router.catalog().add_thermal_camera_lister("zzz", [&lister_calls] {
            ++lister_calls;
            alpacacore::util::ThermalCameraCandidate mini;
            mini.index = 0;
            mini.name = "ZWO ASI290MM Mini";
            mini.serial = "2d0bd20224000900";
            alpacacore::util::ThermalCameraCandidate pro;
            pro.index = 1;
            pro.name = "ZWO ASI2600MM Pro";
            pro.serial = "2e18870f28000900";
            pro.has_thermal = true;
            return std::vector<alpacacore::util::ThermalCameraCandidate>{mini, pro};
        });
        const auto response =
            route_request(router, "GET", "/management/v1/thermalcameras?vendor=zzz&ClientTransactionID=31");
        EXPECT(response.status_code() == 200);
        const auto json = nlohmann::json::parse(response.body(), nullptr, false);
        EXPECT(!json.is_discarded());
        EXPECT(json.value("ErrorNumber", -1) == 0);
        EXPECT(json.value("ClientTransactionID", 0) == 31);
        EXPECT(lister_calls == 1);
        EXPECT(json.contains("Value") && json["Value"].is_array() && json["Value"].size() == 2);
        if (json.contains("Value") && json["Value"].size() == 2) {
            const auto& first = json["Value"][0];
            EXPECT(first.value("Index", -1) == 0);
            EXPECT(first.value("Name", "") == "ZWO ASI290MM Mini");
            EXPECT(first.value("Serial", "") == "2d0bd20224000900");
            EXPECT(first.value("HasThermal", true) == false);
            const auto& second = json["Value"][1];
            EXPECT(second.value("Index", -1) == 1);
            EXPECT(second.value("Name", "") == "ZWO ASI2600MM Pro");
            EXPECT(second.value("HasThermal", false) == true);
        }
    }
    {
        // case: thermalcameras refuses a vendor with no lister and any verb but GET
        alpacahttp::Router router;
        router.catalog().add_thermal_camera_lister(
            "zzz", [] { return std::vector<alpacacore::util::ThermalCameraCandidate>{}; });
        const auto unknown = nlohmann::json::parse(
            route_request(router, "GET", "/management/v1/thermalcameras?vendor=nope").body(), nullptr, false);
        EXPECT(!unknown.is_discarded() && unknown.value("ErrorNumber", 0) == 0x401);
        const auto missing =
            nlohmann::json::parse(route_request(router, "GET", "/management/v1/thermalcameras").body(), nullptr, false);
        EXPECT(!missing.is_discarded() && missing.value("ErrorNumber", 0) == 0x401);
        const auto empty = nlohmann::json::parse(
            route_request(router, "GET", "/management/thermalcameras?vendor=zzz").body(), nullptr, false);
        EXPECT(!empty.is_discarded() && empty.value("ErrorNumber", -1) == 0 && empty["Value"].empty());
        const auto put = route_request(router, "PUT", "/management/v1/thermalcameras", "vendor=zzz");
        EXPECT(put.status_code() == 405);
        EXPECT(put.get_header("Allow") == "GET");
        const auto put_json = nlohmann::json::parse(put.body(), nullptr, false);
        EXPECT(!put_json.is_discarded() && put_json.value("ErrorNumber", 0) == 0x40B);
    }
    {
        // case: thermalcameras reports a throwing lister as an Alpaca error
        alpacahttp::Router router;
        router.catalog().add_thermal_camera_lister(
            "zzz", []() -> std::vector<alpacacore::util::ThermalCameraCandidate> {
                throw alpacacore::AlpacaException("SDK listing failed", alpacacore::AlpacaError::DriverException);
            });
        const auto response = route_request(router, "GET", "/management/v1/thermalcameras?vendor=zzz");
        EXPECT(response.status_code() == 200);
        const auto json = nlohmann::json::parse(response.body(), nullptr, false);
        EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) != 0);
        EXPECT(json.value("ErrorMessage", "").find("SDK listing failed") != std::string::npos);
    }

    {
        // case: thermal switch camera bindings refuse a bad serial or an index below Auto
        alpacahttp::Router router;
        const auto refused = [&router](nlohmann::json body) {
            const auto json = nlohmann::json::parse(
                route_request(router, "POST", "/management/v1/configuredevice", body.dump()).body(), nullptr, false);
            return !json.is_discarded() && json.value("ErrorNumber", 0) != 0;
        };
#ifdef ALPACACORE_ENABLE_ZWO
        EXPECT(refused({{"vendor", "zwo"},
                        {"deviceType", "switch"},
                        {"deviceNumber", 9601},
                        {"switchType", "dewheater"},
                        {"cameraIndex", 1},
                        {"serialNumber", "not-hex"}}));
        EXPECT(refused({{"vendor", "zwo"},
                        {"deviceType", "switch"},
                        {"deviceNumber", 9602},
                        {"switchType", "dewheater"},
                        {"cameraIndex", -2}}));
        // No index at all is Auto on the API path, and registers. A run that
        // aborted before the removal below left 9603 in the persisted file, so
        // clear it first or every later run fails here.
        nlohmann::json remove_body = {{"vendor", "zwo"}, {"deviceType", "switch"}, {"deviceNumber", 9603}};
        route_request(router, "POST", "/management/v1/removedevice", remove_body.dump());
        EXPECT(!refused(
            {{"vendor", "zwo"}, {"deviceType", "switch"}, {"deviceNumber", 9603}, {"switchType", "dewheater"}}));
        EXPECT(nlohmann::json::parse(
                   route_request(router, "POST", "/management/v1/removedevice", remove_body.dump()).body())
                   .value("ErrorNumber", -1) == 0);
#endif
#ifdef ALPACACORE_ENABLE_TOUPTEK
        EXPECT(refused({{"vendor", "touptek"},
                        {"deviceType", "switch"},
                        {"deviceNumber", 9661},
                        {"switchType", "thermal"},
                        {"cameraIndex", -2}}));
#endif
#ifdef ALPACACORE_ENABLE_PLAYERONE
        EXPECT(refused({{"vendor", "playerone"},
                        {"deviceType", "switch"},
                        {"deviceNumber", 9604},
                        {"cameraIndex", 0},
                        {"serialNumber", "POA\nforged"}}));
        EXPECT(
            refused({{"vendor", "playerone"}, {"deviceType", "switch"}, {"deviceNumber", 9605}, {"cameraIndex", -2}}));
#endif
    }

    {
        // case: router hands a catalog bind hook the stored peers and merges what it learns
        alpacahttp::Router router;
        alpacahttp::test_catalog::add_schema_and_bind_factory(router.catalog());
        const auto configure = [&](int number, int count) {
            return api_attempt(router,
                               nlohmann::json{{"vendor", "zzz"},
                                              {"deviceType", "focuser"},
                                              {"deviceNumber", number},
                                              {"count", count},
                                              {"ratio", 3.0}},
                               "Focuser");
        };
        // First entry: nothing stored yet, so no peers; the hook's learned host is
        // stored and its erased ratio is dropped from the saved entry.
        const auto first = configure(9260, 2);
        EXPECT(first.ok);
        EXPECT(first.config.value("host", std::string()) == "peers=0");
        EXPECT(!first.config.contains("ratio"));
        // Second entry: the stored first entry is a peer (count 2 already used).
        const auto clash = configure(9261, 2);
        EXPECT(!clash.ok);
        EXPECT(clash.message == "Invalid value for count: another zzz focuser already uses it");
        EXPECT(listed_entry(router, "Focuser", 9261).is_null());
        const auto second = configure(9261, 3);
        EXPECT(second.ok);
        EXPECT(second.config.value("host", std::string()) == "peers=1");
        remove_device(router, "zzz", "focuser", 9260);
        remove_device(router, "zzz", "focuser", 9261);
    }

    {
        // case: every configured device gets a persisted GUID as its UniqueID (#1471)
        alpacahttp::Router router;
        alpacahttp::test_catalog::add_schema_and_factory(router.catalog());
        const auto configure = [&](int number, const std::optional<std::string>& unique_id) {
            nlohmann::json body = {{"vendor", "zzz"}, {"deviceType", "focuser"}, {"deviceNumber", number}};
            if (unique_id) body["uniqueId"] = *unique_id;
            return api_attempt(router, body, "Focuser");
        };
        const auto listed_unique_id = [&](int number) {
            return listed_entry(router, "Focuser", number).value("UniqueID", std::string());
        };
        const auto first = configure(9471, std::nullopt);
        EXPECT(first.ok);
        const auto stored_id = [&](int number) {
            return raw_listed_config(router, "Focuser", number).value("uniqueId", std::string());
        };
        const std::string first_id = stored_id(9471);
        EXPECT(alpacacore::util::is_unique_id(first_id));
        EXPECT(listed_unique_id(9471) == first_id);  // the driver reports the stored ID

        // An ID another entry holds, or a device-number ID, is replaced.
        const auto copy = configure(9472, first_id);
        EXPECT(copy.ok);
        EXPECT(alpacacore::util::is_unique_id(stored_id(9472)));
        EXPECT(stored_id(9472) != first_id);
        // A stored-form ID (ZWO_UID_) is kept; a second entry sending it, or a device-number ID, gets a GUID.
        const auto legacy = configure(9473, std::string("ZWO_UID_00112233445566ff"));
        EXPECT(legacy.ok);
        EXPECT(stored_id(9473) == "ZWO_UID_00112233445566ff");
        const auto dup = configure(9475, std::string("ZWO_UID_00112233445566ff"));
        EXPECT(dup.ok);
        EXPECT(alpacacore::util::is_unique_id(stored_id(9475)));
        const auto slot = configure(9474, std::string("SkyWatcher_0"));
        EXPECT(slot.ok);
        EXPECT(alpacacore::util::is_unique_id(stored_id(9474)));
        // The web UI's edit of the ZWO_UID_ entry keeps it too.
        remove_device(router, "zzz", "focuser", 9473);
        EXPECT(configure(9473, std::string("ZWO_UID_00112233445566ff")).ok);
        EXPECT(stored_id(9473) == "ZWO_UID_00112233445566ff");

        // The web UI's edit removes the entry and re-adds it with its GUID: it is kept.
        remove_device(router, "zzz", "focuser", 9471);
        const auto edited = configure(9471, first_id);
        EXPECT(edited.ok);
        EXPECT(stored_id(9471) == first_id);
        EXPECT(listed_unique_id(9471) == first_id);
        for (const int number : {9471, 9472, 9473, 9474, 9475}) remove_device(router, "zzz", "focuser", number);
    }
    {
        // case: a stored entry gets a GUID once at load and keeps it across restarts (#1471)
        // The zzz factory is added after the constructor's load, so these entries
        // fail to load: faileddevices still lists each with its GUID.
        const ScopedCwd scratch;
        const std::filesystem::path file = std::filesystem::path("config") / "registered_devices.json";
        std::filesystem::create_directories(file.parent_path());
        const std::string held = alpacacore::util::generate_unique_id();
        {
            std::ofstream out(file, std::ios::trunc);
            out << nlohmann::json::array(
                       {{{"vendor", "zzz"}, {"deviceType", "focuser"}, {"deviceNumber", 9474}},
                        {{"vendor", "zzz"},
                         {"deviceType", "focuser"},
                         {"deviceNumber", 9475},
                         {"uniqueId", "ZWO_UID_00112233445566ff"}},
                        {{"vendor", "zzz"}, {"deviceType", "focuser"}, {"deviceNumber", 9476}, {"uniqueId", held}},
                        {{"vendor", "zzz"}, {"deviceType", "focuser"}, {"deviceNumber", 9477}, {"uniqueId", held}}})
                       .dump();
        }
        std::vector<std::string> first_ids;
        {
            alpacahttp::Router first;
            std::ifstream in(file);
            const auto stored = nlohmann::json::parse(in);
            EXPECT(stored.is_array() && stored.size() == 4);
            std::set<std::string> distinct;
            for (const auto& entry : stored) {
                const std::string id = entry.value("uniqueId", std::string());
                EXPECT(alpacacore::util::is_keepable_unique_id(id));
                first_ids.push_back(id);
                distinct.insert(id);
            }
            EXPECT(distinct.size() == 4);  // the duplicated GUID is replaced in the later entry
            EXPECT(first_ids[1] == "ZWO_UID_00112233445566ff");  // a stored ZWO_UID_ is kept
            EXPECT(first_ids[2] == held);                        // the first holder keeps a valid GUID
            for (int i = 0; i < 4; ++i) {
                const auto row = listed_failed_entry(first, "focuser", 9474 + i);
                EXPECT(!row.is_null());
                EXPECT(row.value("UniqueID", std::string()) == first_ids[static_cast<std::size_t>(i)]);
            }
        }
        {
            alpacahttp::Router again;
            std::ifstream in(file);
            const auto stored = nlohmann::json::parse(in);
            for (std::size_t i = 0; i < 4; ++i) {
                EXPECT(stored[i].value("uniqueId", std::string()) == first_ids[i]);
            }
        }
    }
    {
        // case: a start drops the sync offset of a rotator no longer configured (#1473)
        // Removal cannot drop it, since the web UI's edit removes and re-adds the
        // entry with the same GUID; the next load does (decisions 0008 and 0018).
        const ScopedCwd scratch;
        std::filesystem::create_directories("config");
        const std::string configured = alpacacore::util::generate_unique_id();
        const std::string removed = alpacacore::util::generate_unique_id();
        {
            std::ofstream out(std::filesystem::path("config") / "registered_devices.json", std::ios::trunc);
            out << nlohmann::json::array({{{"vendor", "zzz"},
                                           {"deviceType", "focuser"},
                                           {"deviceNumber", 9478},
                                           {"uniqueId", configured}}})
                       .dump();
        }
        using alpacacore::util::RotatorSyncOffsetStore;
        EXPECT(RotatorSyncOffsetStore::save(configured, 12.5));
        EXPECT(RotatorSyncOffsetStore::save(removed, 7.0));
        EXPECT(RotatorSyncOffsetStore::save("WANDERERASTRO_ROTATOR_3", 3.0));
        { alpacahttp::Router started; }
        EXPECT(RotatorSyncOffsetStore::load(configured) == 12.5);
        EXPECT(RotatorSyncOffsetStore::load(removed) == 0.0);
        EXPECT(RotatorSyncOffsetStore::load("WANDERERASTRO_ROTATOR_3") == 3.0);  // not a GUID key: never pruned
    }
    {
        // case: a start asks only the entries it gives a first GUID to adopt their old state (#1473)
        // An entry that already holds a GUID was configured after the upgrade, maybe
        // on the number of a device removed earlier; it must not take over what that
        // device saved under its driver-built ID (decisions 0008 and 0018).
        const ScopedCwd scratch;
        std::filesystem::create_directories("config");
        {
            std::ofstream out(std::filesystem::path("config") / "registered_devices.json", std::ios::trunc);
            out << nlohmann::json::array({{{"vendor", "zzz"}, {"deviceType", "focuser"}, {"deviceNumber", 9480}},
                                          {{"vendor", "zzz"},
                                           {"deviceType", "focuser"},
                                           {"deviceNumber", 9481},
                                           {"uniqueId", alpacacore::util::generate_unique_id()}}})
                       .dump();
        }
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        const auto unregister = [&] {
            registry.unregister_device(alpacacore::DeviceType::Focuser, 9480);
            registry.unregister_device(alpacacore::DeviceType::Focuser, 9481);
        };
        alpacahttp::test_catalog::adopted_device_numbers().clear();
        {
            alpacahttp::Router started(alpacahttp::test_catalog::add_schema_and_factory);
            EXPECT(alpacahttp::test_catalog::adopted_device_numbers() == std::vector<int>{9480});
        }
        unregister();
        // The first start stored both GUIDs, so a second start asks nobody.
        alpacahttp::test_catalog::adopted_device_numbers().clear();
        { alpacahttp::Router again(alpacahttp::test_catalog::add_schema_and_factory); }
        EXPECT(alpacahttp::test_catalog::adopted_device_numbers().empty());
        unregister();
    }
    {
        // case: a start that cannot store the new GUIDs asks nobody to adopt (#1473)
        // Moving an offset onto a GUID the device file never kept would orphan it:
        // the next start mints another GUID and prunes the moved one.
        const ScopedCwd scratch;
        std::filesystem::create_directories("config");
        {
            std::ofstream out(std::filesystem::path("config") / "registered_devices.json", std::ios::trunc);
            out << nlohmann::json::array({{{"vendor", "zzz"}, {"deviceType", "focuser"}, {"deviceNumber", 9482}}})
                       .dump();
        }
        std::filesystem::permissions("config", std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
        alpacahttp::test_catalog::adopted_device_numbers().clear();
        { alpacahttp::Router read_only(alpacahttp::test_catalog::add_schema_and_factory); }
        std::filesystem::permissions("config", std::filesystem::perms::owner_all);
        EXPECT(alpacahttp::test_catalog::adopted_device_numbers().empty());
        alpacacore::management::DeviceRegistry::instance().unregister_device(alpacacore::DeviceType::Focuser, 9482);
    }
    {
        // case: two configuredevice calls sending one unused GUID do not both keep it (#1471)
        // The first call is held inside its driver's construction, after it chose
        // the GUID and before it stored the entry; the second call must still see
        // the GUID as taken.
        const ScopedCwd scratch;
        struct Gate {
            std::mutex mutex;
            std::condition_variable cv;
            bool entered = false;
            bool released = false;
        };
        auto gate = std::make_shared<Gate>();
        const auto gated_create = [gate](const alpacacore::catalog::DeviceConfig&, int n) {
            if (n == 9483) {
                std::unique_lock<std::mutex> lock(gate->mutex);
                gate->entered = true;
                gate->cv.notify_all();
                gate->cv.wait(lock, [&] { return gate->released; });
            }
            return std::unique_ptr<alpacacore::AlpacaDriver>(new alpacahttp::test_catalog::StubFocuser(n, "gated"));
        };
        alpacahttp::Router router([&](alpacacore::catalog::DeviceCatalog& c) {
            c.add(alpacahttp::test_catalog::make_schema());
            c.add(alpacacore::catalog::Factory{alpacahttp::test_catalog::kKey, gated_create});
        });
        const std::string guid = alpacacore::util::generate_unique_id();
        const auto body = [&](int number) {
            return nlohmann::json{
                {"vendor", "zzz"}, {"deviceType", "focuser"}, {"deviceNumber", number}, {"uniqueId", guid}};
        };
        std::thread first([&] { api_attempt(router, body(9483), "Focuser"); });
        {
            std::unique_lock<std::mutex> lock(gate->mutex);
            gate->cv.wait(lock, [&] { return gate->entered; });
        }
        const auto second = api_attempt(router, body(9484), "Focuser");
        {
            std::lock_guard<std::mutex> lock(gate->mutex);
            gate->released = true;
        }
        gate->cv.notify_all();
        first.join();
        EXPECT(second.ok);
        const std::string first_id = listed_entry(router, "Focuser", 9483).value("UniqueID", std::string());
        const std::string second_id = listed_entry(router, "Focuser", 9484).value("UniqueID", std::string());
        EXPECT(first_id == guid);
        EXPECT(second_id != guid);
        EXPECT(alpacacore::util::is_unique_id(second_id));
        remove_device(router, "zzz", "focuser", 9483);
        remove_device(router, "zzz", "focuser", 9484);
    }
    {
        // case: re-configuring a stored entry at its own number keeps the GUID it sends back (#1471)
        // A script that fixes a failed-to-load entry by configuring the same number
        // again, without removing it first, replaces that entry, so the entry's own
        // GUID is not taken by another one.
        const ScopedCwd scratch;
        const std::filesystem::path file = std::filesystem::path("config") / "registered_devices.json";
        std::filesystem::create_directories(file.parent_path());
        const std::string held = alpacacore::util::generate_unique_id();
        {
            std::ofstream out(file, std::ios::trunc);
            out << nlohmann::json::array(
                       {{{"vendor", "zzz"}, {"deviceType", "focuser"}, {"deviceNumber", 9487}, {"uniqueId", held}}})
                       .dump();
        }
        alpacahttp::Router stored;  // no zzz descriptor at load, so the entry fails to load
        EXPECT(listed_entry(stored, "Focuser", 9487).is_null());
        alpacahttp::test_catalog::add_schema_and_factory(stored.catalog());
        const auto again = api_attempt(
            stored, {{"vendor", "zzz"}, {"deviceType", "focuser"}, {"deviceNumber", 9487}, {"uniqueId", held}},
            "Focuser");
        EXPECT(again.ok);
        EXPECT(listed_entry(stored, "Focuser", 9487).value("UniqueID", std::string()) == held);
        remove_device(stored, "zzz", "focuser", 9487);
    }

    // open-astro#664 Part C: the router consults the catalog and has no vendor
    // arm left, so every outcome below is the catalog path.
    {
        alpacahttp::Router router;
        alpacahttp::test_catalog::add_schema_and_factory(router.catalog());

        // API, valid: registered through the catalog factory with the
        // normalized config (the stub's Name reports what it was handed) and
        // listed with the sanitized raw config: undeclared keys, nulls and the
        // secret dropped, a record's undeclared key dropped, and a field whose
        // applies_when does not match ("host" applies when mode is "b") kept.
        const auto ok = api_attempt(
            router,
            nlohmann::json::parse(R"({"vendor":"zzz","deviceType":"focuser","deviceNumber":9255,"count":3,"mode":"a",)"
                                  R"("host":"h","token":"s3cret","junk":1,"ratio":null,)"
                                  R"("ports":[{"name":"p0","pwm":true,"junk":2}]})"),
            "Focuser");
        EXPECT(ok.ok);
        const auto expected_config =
            nlohmann::json::parse(R"({"vendor":"zzz","deviceType":"focuser","deviceNumber":9255,"count":3,"mode":"a",)"
                                  R"("host":"h","ports":[{"name":"p0","pwm":true}]})");
        EXPECT(ok.config.dump() == expected_config.dump());
        EXPECT(listed_entry(router, "Focuser", 9255).value("DeviceName", "") == "zzz count=3 mode=a");
        remove_device(router, "zzz", "focuser", 9255);

        // The vendor-agnostic keys sanitize_device_config() keeps for every
        // device (responseTimeoutMs, site, optics, syncTimeOnConnect) survive
        // a catalog-sanitized save too, as they did through the deleted arm.
        const auto shared = api_attempt(
            router,
            nlohmann::json::parse(R"({"vendor":"zzz","deviceType":"focuser","deviceNumber":9255,"count":3,)"
                                  R"("responseTimeoutMs":2500,"apertureDiameter":0.2,"focalLength":1.0,)"
                                  R"("siteLatitude":-33.856784,"siteLongitude":151.215297,"siteElevation":30.0,)"
                                  R"("learnSiteFromClient":true,"syncTimeOnConnect":false})"),
            "Focuser");
        EXPECT(shared.ok);
        EXPECT(shared.config.value("count", -1) == 3);
        EXPECT(shared.config.value("responseTimeoutMs", -1) == 2500);
        EXPECT(shared.config.value("apertureDiameter", -1.0) == 0.2);
        EXPECT(shared.config.value("focalLength", -1.0) == 1.0);
        EXPECT(shared.config.value("siteLatitude", 0.0) == -33.856784);
        EXPECT(shared.config.value("siteLongitude", 0.0) == 151.215297);
        EXPECT(shared.config.value("siteElevation", 0.0) == 30.0);
        EXPECT(shared.config.value("learnSiteFromClient", false) == true);
        EXPECT(shared.config.value("syncTimeOnConnect", true) == false);
        remove_device(router, "zzz", "focuser", 9255);

        // API, wrong type: the bridge's InvalidValue is reported as a
        // wrong-type refusal, and nothing registers.
        const auto wrong = api_attempt(
            router,
            nlohmann::json::parse(R"({"vendor":"zzz","deviceType":"focuser","deviceNumber":9255,"count":"seven"})"),
            "Focuser");
        EXPECT(!wrong.ok);
        EXPECT(wrong.message == "Device config field 'count' has the wrong type (got string)");
        EXPECT(listed_entry(router, "Focuser", 9255).is_null());

        // API, out of range: normalize's rejection is the error message, the
        // outcome the deleted reject_invalid_config() produced, and nothing registers.
        const auto rejected = api_attempt(
            router, nlohmann::json::parse(R"({"vendor":"zzz","deviceType":"focuser","deviceNumber":9255,"count":99})"),
            "Focuser");
        EXPECT(!rejected.ok);
        EXPECT(rejected.message.find("count is out of range") != std::string::npos);
        EXPECT(listed_entry(router, "Focuser", 9255).is_null());

        // A key with no schema is refused as not yet supported.
        const auto unknown = api_attempt(
            router, nlohmann::json::parse(R"({"vendor":"yyy","deviceType":"focuser","deviceNumber":9255})"), "Focuser");
        EXPECT(!unknown.ok);
        EXPECT(unknown.message == "Vendor/device type combination not yet supported: yyy/focuser");
    }
    {
        // Persisted: the catalog consult's own warning text, one line per
        // warning -- distinct from the deleted reject_invalid_config()'s "will
        // refuse to connect" wording (that text is false here: normalize() has already
        // substituted a usable value, so the device is not refusing anything).
        // The device registers anyway with the normalized config (count
        // dropped to its default, mode substituted) while configureddevices
        // keeps showing the raw values, secret excluded, so the entry stays
        // editable.
        const auto persisted = persisted_attempt(
            nlohmann::json::parse(
                R"({"vendor":"zzz","deviceType":"focuser","deviceNumber":9256,"count":99,"mode":"zzz","token":"s3cret"})"),
            "Focuser", alpacahttp::test_catalog::add_schema_and_factory);
        EXPECT(persisted.listed);
        EXPECT(persisted.name == "zzz count=1 mode=a");
        EXPECT(persisted.config.value("count", -1) == 99);
        EXPECT(persisted.config.value("mode", "") == "zzz");
        EXPECT(!persisted.config.contains("token"));
        EXPECT(any_warning_contains(
            persisted.warnings,
            "Persisted zzz focuser 9256 config normalized: count is out of range (min 1) (max 8). "
            "The saved value is not used: the field falls back to its default, or stays unset if it has none. "
            "Registered so it stays listed and editable in the web UI."));
        EXPECT(any_warning_contains(
            persisted.warnings,
            "Persisted zzz focuser 9256 config normalized: mode must be one of: a, b. "
            "The saved value is not used: the field falls back to its default, or stays unset if it has none. "
            "Registered so it stays listed and editable in the web UI."));
        EXPECT(!any_warning_contains(persisted.warnings, "will refuse to connect"));
        EXPECT(persisted.errors.empty());
    }
    {
        // Persisted, wrong JSON type: the bridge's InvalidValue is thrown before
        // normalize() runs, so the entry fails to load as a wrong-type refusal.
        // It is not normalized to the default.
        const auto persisted = persisted_attempt(
            nlohmann::json::parse(R"({"vendor":"zzz","deviceType":"focuser","deviceNumber":9258,"count":"seven"})"),
            "Focuser", alpacahttp::test_catalog::add_schema_and_factory);
        EXPECT(!persisted.listed);
        EXPECT(persisted.failed_listed);
        EXPECT(!any_warning_contains(persisted.warnings, "config normalized"));
    }

#ifndef ALPACACORE_ENABLE_ALTAIR
    // With the vendor built out, the catalog path reports "<label> support not
    // enabled", the label being the display name's first word.
    {
        // case: altair built out reports support not enabled
        alpacahttp::Router router;
        const auto off = api_attempt(
            router,
            nlohmann::json::parse(R"({"vendor":"altair","deviceType":"camera","deviceNumber":9255,"cameraIndex":0})"),
            "Camera");
        EXPECT(!off.ok);
        EXPECT(off.message == "Altair support not enabled. Rebuild with -DALPACACORE_ENABLE_ALTAIR=ON");
        EXPECT(listed_entry(router, "Camera", 9255).is_null());
    }
#endif

#ifndef ALPACACORE_ENABLE_ASTROASIS
    // open-astro#664 Part C step 3: with the vendor built out, the catalog path
    // reports the deleted arm's exact text. Green before the arm is deleted;
    // it pins the client-facing and web-UI text across the move.
    {
        alpacahttp::Router router;
        const auto off = api_attempt(
            router,
            nlohmann::json::parse(
                R"({"vendor":"astroasis","deviceType":"focuser","deviceNumber":9257,"hidPath":"/dev/hidraw3"})"),
            "Focuser");
        EXPECT(!off.ok);
        EXPECT(off.message == "Astroasis support not enabled. Rebuild with -DALPACACORE_ENABLE_ASTROASIS=ON");
        EXPECT(listed_entry(router, "Focuser", 9257).is_null());
    }
#endif

#ifndef ALPACACORE_ENABLE_SKYWATCHER
    // open-astro#744 rule 1 / test 3: with the vendor built out, the catalog
    // path reports the deleted arm's exact text through vendor_label() (the
    // display name's first word is "SkyWatcher"). Green before the arm is
    // deleted; it pins the client-facing and web-UI text across the move. The
    // site is supplied because the catalog validates (rule 4) before it
    // answers "not enabled", where the arm answered first.
    {
        alpacahttp::Router router;
        const auto off = api_attempt(router,
                                     nlohmann::json::parse(R"({"vendor":"skywatcher","deviceType":"telescope",)"
                                                           R"("deviceNumber":9258,"connectionType":"serial",)"
                                                           R"("portPath":"/dev/ttyUSB6","siteLatitude":51.477833,)"
                                                           R"("siteLongitude":-0.0015})"),
                                     "Telescope");
        EXPECT(!off.ok);
        EXPECT(off.message == "SkyWatcher support not enabled. Rebuild with -DALPACACORE_ENABLE_SKYWATCHER=ON");
        EXPECT(listed_entry(router, "Telescope", 9258).is_null());
    }
#endif

    // open-astro#392: Host allowlist against DNS rebinding. After a rebind
    // the browser sends the attacker's name as Host (and as Origin), so the
    // Origin==Host guard passes; route() now refuses any Host that is not an
    // IP literal, a reserved local name, the machine's own name or a
    // configured one, for every method and path. The check is opt-in
    // (http.host_check_enabled), so this block turns it on.
    {
        alpacahttp::Router router;
        router.set_host_check_enabled(true);
        const std::string apiversions = "/management/apiversions";

        char hostname_buffer[256] = {};
        EXPECT(::gethostname(hostname_buffer, sizeof(hostname_buffer) - 1) == 0);
        std::string hostname = hostname_buffer;
        std::transform(hostname.begin(), hostname.end(), hostname.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        const std::vector<std::optional<std::string>> allowed = {
            "192.168.1.20",     "192.168.1.20:6800", "[::1]",       "[fe80::1]:6800", "localhost",   "LOCALHOST.",
            hostname,           hostname + ".local", "astro.local", "foo.home.arpa",  "pi.internal", "x.localhost",
            "Astro.Local:6800", std::nullopt,
        };
        for (const auto& host : allowed) {
            const auto response = route_with_host(router, "GET", apiversions, host);
            if (response.status_code() != 200) {
                std::cerr << "Host allowlist: refused " << host.value_or("<no Host>") << "\n";
            }
            EXPECT(response.status_code() == 200);
        }

        // Refused on a GET management route, a PUT device method and a
        // static file alike: the check runs before any of them.
        EXPECT(is_host_refusal(route_with_host(router, "GET", "/management/v1/configureddevices", "attacker.example"),
                               "attacker.example"));
        EXPECT(is_host_refusal(route_with_host(router, "PUT", "/api/v1/telescope/0/tracking", "attacker.example"),
                               "attacker.example"));
        EXPECT(
            is_host_refusal(route_with_host(router, "GET", "/web/index.html", "attacker.example"), "attacker.example"));
        EXPECT(is_host_refusal(route_with_host(router, "GET", "/", "attacker.example:6800"), "attacker.example:6800"));
        // IP-literal tests parse, they do not pattern-match.
        for (const std::string host :
             {"1.2.3.4.evil.example", "0x7f.1", "::1", "fe80::1:6800", "evil.local.example.com",
              "localhost.evil.example", "[::1].evil.example", "[not-an-ip]", "local"}) {
            const auto response = route_with_host(router, "GET", apiversions, host);
            if (!is_host_refusal(response, host)) {
                std::cerr << "Host allowlist: allowed " << host << "\n";
            }
            EXPECT(is_host_refusal(response, host));
        }
        // The echoed name is cut to 255 bytes.
        {
            const std::string long_host(300, 'a');
            EXPECT(is_host_refusal(route_with_host(router, "GET", apiversions, long_host), long_host.substr(0, 255)));
        }

        // The 403 echoes ClientTransactionID: the query string's, else the
        // JSON body's (open-astro#384, #509).
        {
            const auto query_response =
                route_with_host(router, "GET", apiversions + "?ClientTransactionID=4242", "attacker.example");
            EXPECT(is_host_refusal(query_response, "attacker.example"));
            const auto from_query = nlohmann::json::parse(query_response.body(), nullptr, false);
            EXPECT(!from_query.is_discarded() && from_query.value("ClientTransactionID", 0) == 4242);
            const auto body_response =
                route_with_host(router, "PUT", "/api/v1/telescope/0/tracking", "attacker.example",
                                R"({"Tracking": true, "ClientTransactionID": 777})");
            EXPECT(is_host_refusal(body_response, "attacker.example"));
            const auto from_body = nlohmann::json::parse(body_response.body(), nullptr, false);
            EXPECT(!from_body.is_discarded() && from_body.value("ClientTransactionID", 0) == 777);
        }

        // A leading dot is a suffix entry; no dot matches only that name.
        // Entries are normalized like the request host.
        router.set_allowed_hosts({".LAN.", "astropi.home", " ", ""});
        EXPECT(route_with_host(router, "GET", apiversions, "pi.lan").status_code() == 200);
        EXPECT(route_with_host(router, "GET", apiversions, "lan").status_code() == 200);
        EXPECT(route_with_host(router, "GET", apiversions, "Pi.Lan:6800").status_code() == 200);
        EXPECT(route_with_host(router, "GET", apiversions, "astropi.home").status_code() == 200);
        EXPECT(is_host_refusal(route_with_host(router, "GET", apiversions, "lan.evil.example"), "lan.evil.example"));
        EXPECT(is_host_refusal(route_with_host(router, "GET", apiversions, "evillan"), "evillan"));
        EXPECT(is_host_refusal(route_with_host(router, "GET", apiversions, "x.astropi.home"), "x.astropi.home"));
        EXPECT(is_host_refusal(route_with_host(router, "GET", apiversions, "attacker.example"), "attacker.example"));
        // Replacing the list drops the old entries.
        router.set_allowed_hosts({});
        EXPECT(is_host_refusal(route_with_host(router, "GET", apiversions, "pi.lan"), "pi.lan"));
    }

    // The Host check is off by default: a router or reverse-proxy name such as
    // astropi.lan is served, the API and the static UI alike. Turning it on
    // on the live Router changes the next request, turning it off restores it.
    // The Origin==Host guard does not depend on the flag.
    {
        alpacahttp::Router router;
        const std::string apiversions = "/management/apiversions";
        EXPECT(route_with_host(router, "GET", "/management/v1/description", "astropi.lan").status_code() == 200);
        EXPECT(route_with_host(router, "GET", "/", "astropi.lan").status_code() == 200);
        EXPECT(route_with_host(router, "GET", apiversions, "attacker.example").status_code() == 200);

        router.set_host_check_enabled(true);
        EXPECT(is_host_refusal(route_with_host(router, "GET", apiversions, "astropi.lan"), "astropi.lan"));
        EXPECT(is_host_refusal(route_with_host(router, "GET", "/", "astropi.lan"), "astropi.lan"));

        router.set_host_check_enabled(false);
        EXPECT(route_with_host(router, "GET", apiversions, "astropi.lan").status_code() == 200);

        // Origin differs from Host: refused with the check off.
        std::ostringstream raw;
        raw << "DELETE /management/v1/synctime HTTP/1.1\r\n"
            << "Host: astropi.lan\r\n"
            << "Origin: http://evil.example\r\n\r\n";
        alpacahttp::Request request;
        EXPECT(request.parse(raw.str()));
        EXPECT(router.route(request, 1).status_code() == 403);
    }

    // #1509: DiscoveryPort is a server setting next to HostCheckEnabled. GET
    // reports it (32227 by default), PUT validates 1-65535, writes it to the
    // config file's discovery: section and leaves the rest of the file alone.
    // The responder binds it on the next restart.
    {
        // case: DiscoveryPort in the management description is read, validated 1-65535 and saved under discovery
        const std::string description = "/management/v1/description";

        char path_template[] = "/tmp/alpacahttp_test_routing_discport_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        ::close(fd);
        const std::string config_path = path_template;
        const std::string original_file =
            "http:\n"
            "  port: 6811\n"
            "discovery:\n"
            "  enabled: true\n";
        {
            std::ofstream out(config_path);
            out << original_file;
        }
        const auto file_text = [&config_path]() {
            std::ifstream in(config_path);
            std::stringstream buf;
            buf << in.rdbuf();
            return buf.str();
        };
        const auto reported = [&description](alpacahttp::Router& router) {
            const auto response = route_with_host(router, "GET", description, std::nullopt);
            EXPECT(response.status_code() == 200);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded());
            // Additive: the spec keys stay.
            EXPECT(json["Value"].contains("ServerName") && json["Value"].contains("Manufacturer"));
            EXPECT(json["Value"].contains("DiscoveryPort") && json["Value"]["DiscoveryPort"].is_number_integer());
            return json["Value"]["DiscoveryPort"].get<int>();
        };

        alpacahttp::Router router;
        router.set_config_path(config_path);
        EXPECT(reported(router) == 32227);

        for (const char* bad :
             {R"({"DiscoveryPort": 0})", R"({"DiscoveryPort": 65536})", R"({"DiscoveryPort": -1})",
              R"({"DiscoveryPort": "abc"})", R"({"DiscoveryPort": 1.5})", R"({"DiscoveryPort": true})"}) {
            const auto response = route_with_host(router, "PUT", description, std::nullopt, bad);
            EXPECT(response.status_code() == 400);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded());
            EXPECT(json.value("ErrorNumber", 0) == 0x401);
            EXPECT(json.value("ErrorMessage", "") == "DiscoveryPort must be an integer from 1 to 65535");
            EXPECT(file_text() == original_file);
        }
        EXPECT(reported(router) == 32227);

        const auto saved = route_with_host(router, "PUT", description, std::nullopt, R"({"DiscoveryPort": 40000})");
        EXPECT(saved.status_code() == 200);
        EXPECT(reported(router) == 40000);
        EXPECT(file_text() == "http:\n  port: 6811\ndiscovery:\n  enabled: true\n  port: \"40000\"\n");

        // A numeric string, the form the web UI's number input could send.
        EXPECT(
            route_with_host(router, "PUT", description, std::nullopt, R"({"DiscoveryPort": "41000"})").status_code() ==
            200);
        EXPECT(reported(router) == 41000);

        ::unlink(config_path.c_str());
    }

    // The Host check is a server setting: PUT /management/v1/description takes
    // HostCheckEnabled and AllowedHosts, writes them to the config file's
    // http: section and applies them to the next request; GET reports them.
    // A PUT whose own Host the resulting settings would refuse changes
    // nothing, so the operator cannot lock the web UI out from the web UI.
    {
        const std::string description = "/management/v1/description";
        const std::string apiversions = "/management/apiversions";

        char path_template[] = "/tmp/alpacahttp_test_routing_hostcheck_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        ::close(fd);
        const std::string config_path = path_template;
        const std::string original_file =
            "# AlpacaHTTP\n"
            "http:\n"
            "  port: 6811\n"
            "server:\n"
            "  location: \"Backyard\"\n"
            "  allowed_hosts: wrong.section\n"
            "discovery:\n"
            "  enabled: true\n";
        {
            std::ofstream out(config_path);
            out << original_file;
        }
        const auto file_text = [&config_path]() {
            std::ifstream in(config_path);
            std::stringstream buf;
            buf << in.rdbuf();
            return buf.str();
        };
        const auto error_number = [](const alpacahttp::Response& response) {
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            return json.is_discarded() ? -1 : json.value("ErrorNumber", -1);
        };
        // HTTP 400 InvalidValue with exactly this message.
        const auto is_refused_save = [](const alpacahttp::Response& response, const std::string& message) {
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            if (!json.is_discarded() && json.value("ErrorMessage", "") != message) {
                std::cerr << "Host check settings: got '" << json.value("ErrorMessage", "") << "'\n";
            }
            return response.status_code() == 400 && !json.is_discarded() && json.value("ErrorNumber", 0) == 0x401 &&
                   json.value("ErrorMessage", "") == message;
        };
        const auto lockout_message = [](const std::string& host) {
            return "Host '" + host +
                   "' would be refused by these settings; add it to the allowed host names or use the IP address";
        };
        const auto settings = [&description](alpacahttp::Router& router, const std::optional<std::string>& host) {
            const auto response = route_with_host(router, "GET", description, host);
            EXPECT(response.status_code() == 200);
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", -1) == 0);
            return json["Value"];
        };

        alpacahttp::Router router;
        router.set_config_path(config_path);

        // GET reports both values. open-astro#787: nothing marks either one
        // read-only; the web UI can always change them.
        {
            const auto v = settings(router, "astropi.lan");
            EXPECT(v.contains("HostCheckEnabled") && v["HostCheckEnabled"].is_boolean());
            EXPECT(v["HostCheckEnabled"].get<bool>() == false);
            EXPECT(v.contains("AllowedHosts") && v["AllowedHosts"].is_string());
            EXPECT(v["AllowedHosts"].get<std::string>().empty());
            EXPECT(!v.contains("HostCheckEnabledFixedByEnvironment"));
            EXPECT(!v.contains("AllowedHostsFixedByEnvironment"));
        }

        // Turning the check on from a name the list does not hold: 400,
        // nothing changes in memory or in the file.
        EXPECT(
            is_refused_save(route_with_host(router, "PUT", description, "astropi.lan", R"({"HostCheckEnabled": true})"),
                            lockout_message("astropi.lan")));
        EXPECT(route_with_host(router, "GET", apiversions, "attacker.example").status_code() == 200);
        EXPECT(settings(router, "astropi.lan").value("HostCheckEnabled", true) == false);
        EXPECT(file_text() == original_file);
        // The port is not part of the name, and the echoed Host keeps it.
        EXPECT(is_refused_save(
            route_with_host(router, "PUT", description, "AstroPi.lan:6800", R"({"HostCheckEnabled": true})"),
            lockout_message("AstroPi.lan:6800")));
        EXPECT(file_text() == original_file);

        // Both fields in one PUT are judged together: the new list allows the
        // request's Host, so the check turns on and the next request follows.
        EXPECT(error_number(
                   route_with_host(router, "PUT", description, "astropi.lan",
                                   R"({"HostCheckEnabled": true, "AllowedHosts": ".LAN., astropi.home, ,"})")) == 0);
        EXPECT(is_host_refusal(route_with_host(router, "GET", apiversions, "attacker.example"), "attacker.example"));
        EXPECT(is_host_refusal(route_with_host(router, "GET", description, "attacker.example"), "attacker.example"));
        EXPECT(route_with_host(router, "GET", apiversions, "pi.lan").status_code() == 200);
        EXPECT(route_with_host(router, "GET", apiversions, "astropi.home").status_code() == 200);
        {
            // AllowedHosts reads back as the normalized entries, ", " joined.
            const auto v = settings(router, "astropi.lan");
            EXPECT(v.value("HostCheckEnabled", false) == true);
            EXPECT(v.value("AllowedHosts", "") == ".lan, astropi.home");
        }
        {
            // Written under http:, not server:, and the rest of the file is
            // kept. A fresh Config reads the same settings back.
            const std::string text = file_text();
            const auto http_at = text.find("http:\n");
            const auto server_at = text.find("server:\n");
            const auto flag_at = text.find("  host_check_enabled: \"true\"\n");
            const auto list_at = text.find("  allowed_hosts: \".lan, astropi.home\"\n");
            EXPECT(http_at != std::string::npos && server_at != std::string::npos);
            EXPECT(flag_at != std::string::npos && flag_at > http_at && flag_at < server_at);
            EXPECT(list_at != std::string::npos && list_at > http_at && list_at < server_at);
            EXPECT(text.find("  allowed_hosts: wrong.section\n") > server_at);
            EXPECT(text.find("  allowed_hosts: wrong.section\n") != std::string::npos);
            EXPECT(text.find("# AlpacaHTTP\n") == 0);

            alpacahttp::Config reloaded;
            EXPECT(reloaded.load(config_path));
            EXPECT(reloaded.host_check_enabled());
            EXPECT((reloaded.allowed_hosts() == std::vector<std::string>{".lan", "astropi.home"}));
            EXPECT(reloaded.http_port() == 6811);
            EXPECT(reloaded.location() == "Backyard");
            EXPECT(reloaded.discovery_enabled());
        }

        // AllowedHosts alone is judged against the current flag: the check is
        // on, so a list that drops the request's own name is refused.
        {
            const std::string before = file_text();
            EXPECT(is_refused_save(
                route_with_host(router, "PUT", description, "pi.lan", R"({"AllowedHosts": "astropi.home"})"),
                lockout_message("pi.lan")));
            EXPECT(route_with_host(router, "GET", apiversions, "pi.lan").status_code() == 200);
            EXPECT(settings(router, "pi.lan").value("AllowedHosts", "") == ".lan, astropi.home");
            EXPECT(file_text() == before);
            // A refused field refuses the whole request: the profile name
            // that came with it is not applied either.
            EXPECT(is_refused_save(route_with_host(router, "PUT", description, "pi.lan",
                                                   R"({"ProfileName": "Lockout", "AllowedHosts": ""})"),
                                   lockout_message("pi.lan")));
            EXPECT(settings(router, "pi.lan").value("ProfileName", "") != "Lockout");
            EXPECT(file_text() == before);
        }
        // The same list from an IP address, which is always allowed, is
        // accepted; so is a request with no Host header at all.
        EXPECT(error_number(
                   route_with_host(router, "PUT", description, "192.168.1.20:6800", R"({"AllowedHosts": ""})")) == 0);
        EXPECT(is_host_refusal(route_with_host(router, "GET", apiversions, "pi.lan"), "pi.lan"));
        {
            alpacahttp::Config reloaded;
            EXPECT(reloaded.load(config_path));
            EXPECT(reloaded.host_check_enabled());
            EXPECT(reloaded.allowed_hosts().empty());
        }
        EXPECT(error_number(
                   route_with_host(router, "PUT", description, std::nullopt, R"({"allowed_hosts": ".lan"})")) == 0);
        EXPECT(route_with_host(router, "GET", apiversions, "pi.lan").status_code() == 200);

        // Wrong types are InvalidValue and change nothing.
        EXPECT(error_number(route_with_host(router, "PUT", description, "pi.lan", R"({"HostCheckEnabled": 3})")) ==
               0x401);
        EXPECT(error_number(route_with_host(router, "PUT", description, "pi.lan", R"({"AllowedHosts": [".lan"]})")) ==
               0x401);
        EXPECT(error_number(
                   route_with_host(router, "PUT", description, "pi.lan", R"({"HostCheckEnabled": "maybe"})")) == 0x401);
        EXPECT(settings(router, "pi.lan").value("HostCheckEnabled", false) == true);
        EXPECT(settings(router, "pi.lan").value("AllowedHosts", "") == ".lan");

        // Off again (string form, camelCase key): every Host is served, the
        // list is kept, and the file says so.
        EXPECT(error_number(
                   route_with_host(router, "PUT", description, "pi.lan", R"({"hostCheckEnabled": "false"})")) == 0);
        EXPECT(route_with_host(router, "GET", apiversions, "attacker.example").status_code() == 200);
        {
            alpacahttp::Config reloaded;
            EXPECT(reloaded.load(config_path));
            EXPECT(!reloaded.host_check_enabled());
            EXPECT((reloaded.allowed_hosts() == std::vector<std::string>{".lan"}));
            const std::string text = file_text();
            EXPECT(text.find("host_check_enabled") == text.rfind("host_check_enabled"));  // replaced, not appended
        }
        // With the check off the list may be changed from any Host; turning
        // the check on alone is then judged against that stored list.
        EXPECT(error_number(route_with_host(router, "PUT", description, "astropi.lan",
                                            R"({"AllowedHosts": "astropi.home"})")) == 0);
        EXPECT(
            is_refused_save(route_with_host(router, "PUT", description, "astropi.lan", R"({"HostCheckEnabled": true})"),
                            lockout_message("astropi.lan")));
        EXPECT(route_with_host(router, "GET", apiversions, "attacker.example").status_code() == 200);
        EXPECT(error_number(
                   route_with_host(router, "PUT", description, "astropi.home", R"({"HostCheckEnabled": true})")) == 0);
        EXPECT(is_host_refusal(route_with_host(router, "GET", apiversions, "astropi.lan"), "astropi.lan"));

        // A Host that cannot name a host is refused like route() refuses it.
        EXPECT(error_number(
                   route_with_host(router, "PUT", description, "astropi.home", R"({"HostCheckEnabled": false})")) == 0);
        EXPECT(is_refused_save(
            route_with_host(router, "PUT", description, "fe80::1:6800", R"({"HostCheckEnabled": true})"),
            lockout_message("fe80::1:6800")));

        // Cross-origin: 403 from the same guard as the other description
        // fields, and the check stays off.
        {
            const std::string body = R"({"HostCheckEnabled": true})";
            std::ostringstream raw;
            raw << "PUT " << description << " HTTP/1.1\r\n"
                << "Host: localhost\r\n"
                << "Origin: http://evil.example\r\n"
                << "Content-Type: text/plain\r\n"
                << "Content-Length: " << body.size() << "\r\n\r\n"
                << body;
            alpacahttp::Request request;
            EXPECT(request.parse(raw.str()));
            const auto response = router.route(request, 1);
            EXPECT(response.status_code() == 403);
            EXPECT(error_number(response) == 0x401);
            EXPECT(settings(router, "localhost").value("HostCheckEnabled", true) == false);
            EXPECT(route_with_host(router, "GET", apiversions, "attacker.example").status_code() == 200);
        }

        // An entry that cannot match a Host header is refused, not dropped:
        // 400, the file and GET unchanged.
        {
            const std::string before_file = file_text();
            const std::string before_hosts = settings(router, "localhost")["AllowedHosts"].get<std::string>();
            const std::vector<std::string> bad_lists = {"a#b, .lan",
                                                        "*.lan",
                                                        "http://x.lan",
                                                        "a b",
                                                        std::string("a\x01.lan"),
                                                        "a..lan",
                                                        "[::zz]",
                                                        "x.lan:abc",
                                                        std::string("[::1\0junk]", 10)};
            for (const auto& list : bad_lists) {
                nlohmann::json body;
                body["AllowedHosts"] = list;
                const auto response = route_with_host(router, "PUT", description, "localhost", body.dump());
                const auto json = nlohmann::json::parse(response.body(), nullptr, false);
                EXPECT(response.status_code() == 400);
                EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) == 0x401);
                EXPECT(file_text() == before_file);
                EXPECT(settings(router, "localhost")["AllowedHosts"].get<std::string>() == before_hosts);
            }
            std::string many;
            for (int i = 0; i < 65; ++i) {
                many += "h" + std::to_string(i) + ".lan,";
            }
            nlohmann::json too_many;
            too_many["AllowedHosts"] = many;
            EXPECT(route_with_host(router, "PUT", description, "localhost", too_many.dump()).status_code() == 400);
            EXPECT(file_text() == before_file);
        }

        // A valid list, and a location holding '#', survive write and reload.
        {
            EXPECT(error_number(route_with_host(
                       router, "PUT", description, "localhost",
                       R"({"AllowedHosts": ".LAN, astropi.home:8080, [::1], my_host-1.", "Location": "Obs #2"})")) ==
                   0);
            alpacahttp::Config reloaded;
            EXPECT(reloaded.load(config_path));
            EXPECT(
                (reloaded.allowed_hosts() == std::vector<std::string>{".lan", "astropi.home", "[::1]", "my_host-1"}));
            EXPECT(settings(router, "localhost")["AllowedHosts"].get<std::string>() ==
                   ".lan, astropi.home, [::1], my_host-1");
            EXPECT(reloaded.location() == "Obs #2");
            EXPECT(error_number(route_with_host(router, "PUT", description, "localhost", R"({"AllowedHosts": ""})")) ==
                   0);
        }

        // A body with none of the settable properties names all of them.
        {
            const auto response = route_with_host(router, "PUT", description, "localhost", R"({"Unrelated": 1})");
            const auto json = nlohmann::json::parse(response.body(), nullptr, false);
            EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) == 0x402);
            EXPECT(json.value("ErrorMessage", "") ==
                   "Request must include a 'Location', 'ProfileName', 'SyncSystemClockFromClients', "
                   "'HostCheckEnabled', 'AllowedHosts' or 'DiscoveryPort' property");
        }
        ::unlink(config_path.c_str());

        // No file yet: the PUT creates it with both sections, each key under
        // its own.
        {
            alpacahttp::Router created;
            created.set_config_path(config_path);
            EXPECT(error_number(route_with_host(
                       created, "PUT", description, "pi.lan",
                       R"({"Location": "Roof", "HostCheckEnabled": true, "AllowedHosts": ".lan"})")) == 0);
            alpacahttp::Config reloaded;
            EXPECT(reloaded.load(config_path));
            EXPECT(reloaded.host_check_enabled());
            EXPECT((reloaded.allowed_hosts() == std::vector<std::string>{".lan"}));
            EXPECT(reloaded.location() == "Roof");
            ::unlink(config_path.c_str());
        }

        // A file that cannot be written: the PUT fails and memory keeps the
        // old settings.
        {
            alpacahttp::Router unwritable;
            unwritable.set_config_path("/nonexistent-alpacahttp-test-dir/default.yaml");
            const auto response = route_with_host(unwritable, "PUT", description, "localhost",
                                                  R"({"HostCheckEnabled": true, "AllowedHosts": ".lan"})");
            EXPECT(error_number(response) == 0x500);
            EXPECT(route_with_host(unwritable, "GET", apiversions, "attacker.example").status_code() == 200);
            EXPECT(settings(unwritable, "localhost").value("AllowedHosts", "x").empty());
        }

        // No config path (a Router built without a file): applied live, as
        // the other description fields are.
        {
            alpacahttp::Router no_file;
            EXPECT(error_number(route_with_host(no_file, "PUT", description, "localhost",
                                                R"({"HostCheckEnabled": true, "AllowedHosts": ".lan"})")) == 0);
            EXPECT(
                is_host_refusal(route_with_host(no_file, "GET", apiversions, "attacker.example"), "attacker.example"));
            EXPECT(route_with_host(no_file, "GET", apiversions, "pi.lan").status_code() == 200);
        }
    }

    // Saving a setting keeps the trailing comment of the line it replaces
    // (the help text default.yaml ships), and a '#' inside a quoted old value
    // is data, not a comment.
    {
        char path_template[] = "/tmp/alpacahttp_test_routing_comments_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        ::close(fd);
        const std::string config_path = path_template;
        {
            std::ofstream out(config_path);
            out << "http:\n"
                   "  host_check_enabled: false  # help text\n"
                   "  allowed_hosts: \"\"\n"
                   "server:\n"
                   "  location: \"Obs #2\"\n"
                   "  profile_name: \"Old\"   # shown in the UI\n";
        }
        alpacahttp::Router router;
        router.set_config_path(config_path);
        const auto put = [&router](const std::string& body) {
            return route_with_host(router, "PUT", "/management/v1/description", std::nullopt, body);
        };
        EXPECT(put(R"({"HostCheckEnabled": true, "AllowedHosts": ".lan", "Location": "Roof", "ProfileName": "New"})")
                   .status_code() == 200);
        std::ifstream in(config_path);
        std::stringstream buf;
        buf << in.rdbuf();
        const std::string text = buf.str();
        EXPECT(text.find("  host_check_enabled: \"true\"  # help text\n") != std::string::npos);
        EXPECT(text.find("  allowed_hosts: \".lan\"\n") != std::string::npos);
        EXPECT(text.find("  location: \"Roof\"\n") != std::string::npos);
        EXPECT(text.find("  profile_name: \"New\"   # shown in the UI\n") != std::string::npos);
        EXPECT(text.find("Obs") == std::string::npos);

        alpacahttp::Config reloaded;
        EXPECT(reloaded.load(config_path));
        EXPECT(reloaded.host_check_enabled());
        EXPECT((reloaded.allowed_hosts() == std::vector<std::string>{".lan"}));
        EXPECT(reloaded.location() == "Roof");
        EXPECT(reloaded.profile_name() == "New");
        std::remove(config_path.c_str());
    }

    // A blank line or a column-0 comment inside a section does not end it: the
    // existing key is updated in place and no duplicate key is appended.
    {
        const char* const kGaps[] = {"\n", "# note\n"};
        for (const char* gap : kGaps) {
            char path_template[] = "/tmp/alpacahttp_test_routing_gap_XXXXXX";
            int fd = ::mkstemp(path_template);
            EXPECT(fd >= 0);
            ::close(fd);
            const std::string config_path = path_template;
            {
                std::ofstream out(config_path);
                out << "http:\n"
                       "  host_check_enabled: false\n"
                    << gap
                    << "  allowed_hosts: \"\"\n"
                       "\n"
                       "# next section\n"
                       "server:\n"
                       "  location: \"Old\"\n";
            }
            alpacahttp::Router router;
            router.set_config_path(config_path);
            EXPECT(route_with_host(router, "PUT", "/management/v1/description", std::nullopt,
                                   R"({"HostCheckEnabled": true, "AllowedHosts": ".lan"})")
                       .status_code() == 200);
            std::ifstream in(config_path);
            std::stringstream buf;
            buf << in.rdbuf();
            const std::string text = buf.str();
            const auto count = [&text](const std::string& needle) {
                std::size_t n = 0;
                for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) {
                    ++n;
                }
                return n;
            };
            EXPECT(count("host_check_enabled:") == 1);
            EXPECT(count("allowed_hosts:") == 1);
            EXPECT(text.find("  allowed_hosts: \".lan\"\n\n# next section\nserver:\n") != std::string::npos);
            std::remove(config_path.c_str());
        }
    }

    // A key missing from an existing section is added after the section's last
    // content line, ahead of its trailing blank and comment lines.
    {
        char path_template[] = "/tmp/alpacahttp_test_routing_append_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        ::close(fd);
        const std::string config_path = path_template;
        {
            std::ofstream out(config_path);
            out << "http:\n"
                   "  host_check_enabled: false\n"
                   "\n"
                   "# next section\n"
                   "server:\n"
                   "  location: \"Old\"\n";
        }
        alpacahttp::Router router;
        router.set_config_path(config_path);
        EXPECT(route_with_host(router, "PUT", "/management/v1/description", std::nullopt,
                               R"({"HostCheckEnabled": true, "AllowedHosts": ".lan"})")
                   .status_code() == 200);
        std::ifstream in(config_path);
        std::stringstream buf;
        buf << in.rdbuf();
        const std::string text = buf.str();
        EXPECT(text.find("  allowed_hosts: \".lan\"\n\n# next section\nserver:") != std::string::npos);
        std::remove(config_path.c_str());
    }

    // Keys for a section the file lacks go under that section's new header,
    // not into the section the file ends with.
    {
        char path_template[] = "/tmp/alpacahttp_test_routing_new_section_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        ::close(fd);
        const std::string config_path = path_template;
        {
            std::ofstream out(config_path);
            out << "server:\n"
                   "  location: \"Old\"\n";
        }
        alpacahttp::Router router;
        router.set_config_path(config_path);
        EXPECT(route_with_host(router, "PUT", "/management/v1/description", std::nullopt,
                               R"({"HostCheckEnabled": true, "AllowedHosts": ".lan"})")
                   .status_code() == 200);
        std::ifstream in(config_path);
        std::stringstream buf;
        buf << in.rdbuf();
        EXPECT(buf.str() ==
               "server:\n"
               "  location: \"Old\"\n"
               "\n"
               "http:\n"
               "  host_check_enabled: \"true\"\n"
               "  allowed_hosts: \".lan\"\n");
        std::remove(config_path.c_str());
    }

    // A lone double quote inside a plain old value does not hide its comment.
    {
        char path_template[] = "/tmp/alpacahttp_test_routing_lone_quote_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        ::close(fd);
        const std::string config_path = path_template;
        {
            std::ofstream out(config_path);
            out << "server:\n"
                   "  location: 8\" Dob  # note\n";
        }
        alpacahttp::Router router;
        router.set_config_path(config_path);
        EXPECT(route_with_host(router, "PUT", "/management/v1/description", std::nullopt, R"({"Location": "Roof"})")
                   .status_code() == 200);
        std::ifstream in(config_path);
        std::stringstream buf;
        buf << in.rdbuf();
        EXPECT(buf.str().find("  location: \"Roof\"  # note\n") != std::string::npos);
        std::remove(config_path.c_str());
    }

    // A single-quoted old value protects its '#' like a double-quoted one; a
    // single quote inside a plain value does not.
    // The second row's comment starts at its first '#', so "#2  # note" is kept whole.
    const char* const kQuoteRows[][2] = {{"  location: 'Obs #2'  # note\n", "  location: \"Roof\"  # note\n"},
                                         {"  location: Bob's #2  # note\n", "  location: \"Roof\" #2  # note\n"}};
    for (const auto& row : kQuoteRows) {
        char path_template[] = "/tmp/alpacahttp_test_routing_single_quote_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        ::close(fd);
        const std::string config_path = path_template;
        {
            std::ofstream out(config_path);
            out << "server:\n" << row[0];
        }
        alpacahttp::Router router;
        router.set_config_path(config_path);
        EXPECT(route_with_host(router, "PUT", "/management/v1/description", std::nullopt, R"({"Location": "Roof"})")
                   .status_code() == 200);
        std::ifstream in(config_path);
        std::stringstream buf;
        buf << in.rdbuf();
        EXPECT(buf.str() == std::string("server:\n") + row[1]);
        std::remove(config_path.c_str());
    }

#ifdef ALPACACORE_ENABLE_WEEWX
    // open-astro#731: the WeeWX refusals keep the router arm's exact text on
    // the API path, and a persisted entry that breaks one is still not
    // registered (listed as failed to load, so it stays editable). The first
    // three rows were green before the arm moved into the catalog and pin the
    // texts and outcome across it; the last two pin the factory's INT_MAX bound,
    // which keeps the int domain the arm read both numbers in.
    {
        const char* const kBadConfigs[][2] = {
            {R"({"pollIntervalSeconds":300,"timeoutMs":2500})", "WeeWX observing conditions requires weewxUrl"},
            {R"({"weewxUrl":"http://weewx.test:8998/current.json","pollIntervalSeconds":0})",
             "pollIntervalSeconds must be greater than 0"},
            {R"({"weewxUrl":"http://weewx.test:8998/current.json","timeoutMs":0})", "timeoutMs must be greater than 0"},
            {R"({"weewxUrl":"http://weewx.test:8998/current.json","pollIntervalSeconds":10000000000})",
             "pollIntervalSeconds must be at most 2147483647"},
            {R"({"weewxUrl":"http://weewx.test:8998/current.json","timeoutMs":10000000000})",
             "timeoutMs must be at most 2147483647"},
        };
        int number = 9265;
        for (const auto& bad : kBadConfigs) {
            nlohmann::json entry = nlohmann::json::parse(bad[0]);
            entry.update({{"vendor", "weewx"}, {"deviceType", "observingconditions"}, {"deviceNumber", ++number}});

            alpacahttp::Router router;
            const auto api = api_attempt(router, entry, "ObservingConditions");
            EXPECT(!api.ok);
            EXPECT(api.message == bad[1]);
            EXPECT(api.error_number == 0x401);  // InvalidValue
            EXPECT(listed_entry(router, "ObservingConditions", number).is_null());

            const auto persisted = persisted_attempt(entry, "ObservingConditions");
            EXPECT(!persisted.listed);
            EXPECT(persisted.failed_listed);
        }
    }
#else
    // open-astro#731: with the vendor built out, the catalog path reports the
    // deleted arm's exact text.
    {
        alpacahttp::Router router;
        const auto off = api_attempt(
            router,
            nlohmann::json::parse(R"({"vendor":"weewx","deviceType":"observingconditions","deviceNumber":9264,)"
                                  R"("weewxUrl":"http://weewx.test:8998/current.json"})"),
            "ObservingConditions");
        EXPECT(!off.ok);
        EXPECT(off.message == "WeeWX support not enabled. Rebuild with -DALPACACORE_ENABLE_WEEWX=ON");
        EXPECT(off.error_number == 0x400);  // NotImplemented
        EXPECT(listed_entry(router, "ObservingConditions", 9264).is_null());
    }
#endif

    {
        // Persisted: the wrong-type refusal is thrown before normalize() and the
        // availability check, so the entry fails to load (listed as failed) in
        // every build, where the deleted arm loaded 1.5 as index 1.
        const auto persisted = persisted_attempt(
            nlohmann::json::parse(R"({"vendor":"svbony","deviceType":"camera","deviceNumber":9269,"cameraIndex":1.5})"),
            "Camera");
        EXPECT(!persisted.listed);
        EXPECT(persisted.failed_listed);
        EXPECT(any_warning_contains(persisted.errors, "cameraIndex"));
    }

#ifdef ALPACACORE_ENABLE_SVBONY
    // The catalog's Int field refuses what the deleted arm's get<int>()
    // truncated (1.5) or coerced (true): not registered, InvalidValue.
    {
        const char* const kBadIndexes[] = {"1.5", "true"};
        int number = 9266;
        for (const char* bad : kBadIndexes) {
            nlohmann::json entry = nlohmann::json::parse(std::string(R"({"cameraIndex":)") + bad + "}");
            entry.update({{"vendor", "svbony"}, {"deviceType", "camera"}, {"deviceNumber", ++number}});
            alpacahttp::Router router;
            const auto api = api_attempt(router, entry, "Camera");
            EXPECT(!api.ok);
            EXPECT(api.message.find("cameraIndex") != std::string::npos);
            EXPECT(api.error_number == 0x401);  // InvalidValue
            EXPECT(listed_entry(router, "Camera", number).is_null());
        }
    }
#else
    // With the vendor built out, the catalog path reports the deleted arm's text.
    {
        alpacahttp::Router router;
        const auto off = api_attempt(
            router, nlohmann::json::parse(R"({"vendor":"svbony","deviceType":"camera","deviceNumber":9265})"),
            "Camera");
        EXPECT(!off.ok);
        EXPECT(off.message == "SVBONY support not enabled. Rebuild with -DALPACACORE_ENABLE_SVBONY=ON");
        EXPECT(off.error_number == 0x400);  // NotImplemented
        EXPECT(listed_entry(router, "Camera", 9265).is_null());
    }
#endif

#ifndef ALPACACORE_ENABLE_ZWO
    // With the vendor built out, the catalog path reports the deleted arm's text.
    {
        // case: zwo camera vendors-OFF refusal text
        alpacahttp::Router router;
        const auto off = api_attempt(
            router,
            nlohmann::json::parse(R"({"vendor":"zwo","deviceType":"camera","deviceNumber":9266,"cameraIndex":0})"),
            "Camera");
        EXPECT(!off.ok);
        EXPECT(off.message == "ZWO support not enabled. Rebuild with -DALPACACORE_ENABLE_ZWO=ON");
        EXPECT(off.error_number == 0x400);  // NotImplemented
        EXPECT(listed_entry(router, "Camera", 9266).is_null());
    }
#endif

#ifndef ALPACACORE_ENABLE_GPHOTO
    // With the vendor built out, the catalog path reports the deleted arm's text.
    {
        // case: gphoto vendors-OFF refusal text
        alpacahttp::Router router;
        const auto off = api_attempt(
            router, nlohmann::json::parse(R"({"vendor":"gphoto","deviceType":"camera","deviceNumber":9264})"),
            "Camera");
        EXPECT(!off.ok);
        EXPECT(off.message == "gphoto support not enabled. Rebuild with -DALPACACORE_ENABLE_GPHOTO=ON");
        EXPECT(off.error_number == 0x400);  // NotImplemented
        EXPECT(listed_entry(router, "Camera", 9264).is_null());
    }
#endif

#ifndef ALPACACORE_ENABLE_BISQUE
    // With the vendor built out, the catalog path reports the deleted arm's text.
    {
        // case: Bisque vendors-OFF refusal text
        alpacahttp::Router router;
        const auto off = api_attempt(
            router, nlohmann::json{{"vendor", "bisque"}, {"deviceType", "telescope"}, {"deviceNumber", 9268}},
            "Telescope");
        EXPECT(!off.ok);
        EXPECT(off.message == "Bisque support not enabled. Rebuild with -DALPACACORE_ENABLE_BISQUE=ON");
        EXPECT(off.error_number == 0x400);  // NotImplemented
        EXPECT(listed_entry(router, "Telescope", 9268).is_null());
    }
#endif

#ifndef ALPACACORE_ENABLE_CELESTRON
    // With the vendor built out, the catalog path reports the deleted arm's text.
    {
        // case: Celestron vendors-OFF refusal text
        alpacahttp::Router router;
        const auto off = api_attempt(
            router, nlohmann::json{{"vendor", "celestron"}, {"deviceType", "telescope"}, {"deviceNumber", 9272}},
            "Telescope");
        EXPECT(!off.ok);
        EXPECT(off.message == "Celestron support not enabled. Rebuild with -DALPACACORE_ENABLE_CELESTRON=ON");
        EXPECT(off.error_number == 0x400);  // NotImplemented
        EXPECT(listed_entry(router, "Telescope", 9272).is_null());
    }
#endif

#ifndef ALPACACORE_ENABLE_SYNSCAN
    // With the vendor built out, the catalog path reports the deleted arm's text.
    {
        // case: SynScan vendors-OFF refusal text
        alpacahttp::Router router;
        const auto off = api_attempt(
            router, nlohmann::json{{"vendor", "synscan"}, {"deviceType", "telescope"}, {"deviceNumber", 9273}},
            "Telescope");
        EXPECT(!off.ok);
        EXPECT(off.message == "SynScan support not enabled. Rebuild with -DALPACACORE_ENABLE_SYNSCAN=ON");
        EXPECT(off.error_number == 0x400);  // NotImplemented
        EXPECT(listed_entry(router, "Telescope", 9273).is_null());
    }
#endif

#ifndef ALPACACORE_ENABLE_QHY
    // With the vendor built out, the catalog path reports the deleted arms' text.
    {
        // case: QHY vendors-OFF refusal text
        alpacahttp::Router router;
        const auto off = api_attempt(
            router,
            nlohmann::json{{"vendor", "qhy"}, {"deviceType", "camera"}, {"deviceNumber", 9274}, {"cameraIndex", 0}},
            "Camera");
        EXPECT(!off.ok);
        EXPECT(off.message == "QHY support not enabled. Rebuild with -DALPACACORE_ENABLE_QHY=ON");
        EXPECT(listed_entry(router, "Camera", 9274).is_null());
    }
#endif

#ifndef ALPACACORE_ENABLE_ONSTEP
    // With the vendor built out, the catalog path reports the deleted arm's text.
    {
        // case: OnStep vendors-OFF refusal text
        alpacahttp::Router router;
        const auto off = api_attempt(
            router, nlohmann::json{{"vendor", "onstep"}, {"deviceType", "telescope"}, {"deviceNumber", 9271}},
            "Telescope");
        EXPECT(!off.ok);
        EXPECT(off.message == "OnStep support not enabled. Rebuild with -DALPACACORE_ENABLE_ONSTEP=ON");
        EXPECT(off.error_number == 0x400);  // NotImplemented
        EXPECT(listed_entry(router, "Telescope", 9271).is_null());
    }
#endif

#ifndef ALPACACORE_ENABLE_PLAYERONE
    // With the vendor built out, the catalog path reports the deleted arms' text
    // for each Player One type: the two-word label comes from Schema::vendor_label.
    {
        // case: Player One vendors-OFF refusal text
        alpacahttp::Router router;
        const std::pair<const char*, const char*> types[] = {
            {"camera", "Camera"}, {"filterwheel", "FilterWheel"}, {"switch", "Switch"}};
        for (const auto& [device_type, listed_type] : types) {
            const auto off = api_attempt(
                router, nlohmann::json{{"vendor", "playerone"}, {"deviceType", device_type}, {"deviceNumber", 9265}},
                listed_type);
            EXPECT(!off.ok);
            EXPECT(off.message == "Player One support not enabled. Rebuild with -DALPACACORE_ENABLE_PLAYERONE=ON");
            EXPECT(off.error_number == 0x400);  // NotImplemented
            EXPECT(listed_entry(router, listed_type, 9265).is_null());
        }
    }
#endif

    {
        // case: failed-to-load entries are served by faileddevices, not configureddevices (#576)
        const ScopedCwd scratch;
        std::filesystem::create_directories("config");
        {
            std::ofstream out(std::filesystem::path("config") / "registered_devices.json", std::ios::trunc);
            out << R"([{"vendor":"nosuchvendor","deviceType":"focuser","deviceNumber":9577}])";
        }
        alpacahttp::Router router;
        const auto configured = nlohmann::json::parse(
            route_request(router, "GET", "/management/v1/configureddevices").body(), nullptr, false);
        EXPECT(!configured.is_discarded() && configured.value("ErrorNumber", -1) == 0);
        for (const auto& row : configured["Value"]) {
            EXPECT(!row.value("LoadError", false));
            EXPECT(row.value("UniqueID", "x") != "");
            EXPECT(row.value("Vendor", "") != "nosuchvendor");
        }
        for (const std::string path : {"/management/v1/faileddevices", "/management/faileddevices"}) {
            const auto failed = nlohmann::json::parse(route_request(router, "GET", path).body(), nullptr, false);
            EXPECT(!failed.is_discarded() && failed.value("ErrorNumber", -1) == 0);
            EXPECT(failed["Value"].is_array() && failed["Value"].size() == 1);
            const auto& row = failed["Value"][0];
            EXPECT(row.value("LoadError", false));
            EXPECT(row.value("DeviceName", "") == "nosuchvendor (failed to load)");
            EXPECT(row.value("DeviceType", "") == "focuser");
            EXPECT(row.value("DeviceNumber", -1) == 9577);
            // Decision 0018: a failed-to-load entry keeps the ID stored for it.
            EXPECT(alpacacore::util::is_unique_id(row.value("UniqueID", std::string())));
            EXPECT(row.value("UniqueID", std::string()) == row["Config"].value("uniqueId", std::string("x")));
            EXPECT(row["Config"].value("vendor", "") == "nosuchvendor");
        }
        const auto failed_put = route_request(router, "PUT", "/management/v1/faileddevices", "{}");
        EXPECT(failed_put.status_code() == 405);
        EXPECT(failed_put.get_header("Allow") == "GET");
    }
    {
        // case: every AlpacaBridge-only 405 names the methods its handler accepts in Allow (RFC 9110 s15.5.6)
        alpacahttp::Router router;
        const auto expect_allow = [&](const std::string& method, const std::string& path, const std::string& allow) {
            const auto resp = route_request(router, method, path, "");
            EXPECT(resp.status_code() == 405);
            EXPECT(resp.get_header("Allow") == allow);
        };
        expect_allow("DELETE", "/management/v1/configuredevice", "POST, PUT");
        expect_allow("GET", "/management/v1/configuredevice", "POST, PUT");
        expect_allow("GET", "/management/v1/removedevice", "POST, PUT");
        expect_allow("GET", "/management/v1/shutdown", "POST, PUT");
        expect_allow("GET", "/management/v1/restart", "POST, PUT");
        expect_allow("DELETE", "/management/v1/synctime", "GET, POST, PUT");
    }
    {
        // case: routes outside the Alpaca schema are rejected (#576)
        constexpr int kSchemaDevice = 9576;
        class AsyncSwitchStub final : public route_table_stubs::SwitchStub {
        public:
            using SwitchStub::SwitchStub;
            int get_max_switch() const override { return 1; }
            bool get_can_async(int id) const override {
                if (id < 0 || id >= get_max_switch()) {
                    throw alpacacore::AlpacaException("Switch id out of range", alpacacore::AlpacaError::InvalidValue);
                }
                return true;
            }
        };
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::DomeStub>(kSchemaDevice)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::ObservingConditionsStub>(kSchemaDevice)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::TelescopeStub>(kSchemaDevice)));
        EXPECT(registry.register_device(std::make_shared<AsyncSwitchStub>(kSchemaDevice)));
        alpacahttp::Router router;
        const std::string n = std::to_string(kSchemaDevice);
        auto body_of = [&](const std::string& verb, const std::string& path) {
            const auto response = route_request(router, verb, path, verb == "PUT" ? "ClientID=1" : "");
            // A Device API 400 is text/plain, not JSON (#1476): carry its text as ErrorMessage.
            const auto parsed = nlohmann::json::parse(response.body(), nullptr, false);
            return std::make_pair(response.status_code(),
                                  parsed.is_object() ? parsed : nlohmann::json{{"ErrorMessage", response.body()}});
        };

        for (const std::string path :
             {"/api/v1/dome/" + n + "/canslew", "/api/v1/observingconditions/" + n + "/seeing"}) {
            const auto [status, json] = body_of("GET", path);
            EXPECT(status == 400);
            EXPECT(json.value("ErrorMessage", "").rfind("Unknown method: ", 0) == 0);
        }

        // The "mount" alias of telescope is gone; the real path still works.
        {
            const auto [status, json] = body_of("GET", "/api/v1/mount/" + n + "/connected");
            EXPECT(status == 400);
            EXPECT(json.value("ErrorMessage", "") == "Unknown device type: mount");
            const auto [real_status, real_json] = body_of("GET", "/api/v1/telescope/" + n + "/connected");
            EXPECT(real_status == 200);
            EXPECT(real_json.value("ErrorNumber", -1) == 0);
        }

        // A switch has no cancel member: both verbs answer NotImplemented even
        // when the switch is async-capable, never a success that cancelled nothing.
        for (const std::string verb : {"GET", "PUT"}) {
            const auto [status, json] = body_of(verb, "/api/v1/switch/" + n + "/cancelasync?Id=0");
            EXPECT(status == 200);
            EXPECT(json.value("ErrorNumber", -1) == 0x400);
            // An out-of-range Id is still InvalidValue (ConformU checks it).
            const auto [bad_status, bad_json] = body_of(verb, "/api/v1/switch/" + n + "/cancelasync?Id=-1");
            EXPECT(bad_status == 200);
            EXPECT(bad_json.value("ErrorNumber", -1) == 0x401);
        }

        registry.unregister_device(alpacacore::DeviceType::Dome, kSchemaDevice);
        registry.unregister_device(alpacacore::DeviceType::ObservingConditions, kSchemaDevice);
        registry.unregister_device(alpacacore::DeviceType::Telescope, kSchemaDevice);
        registry.unregister_device(alpacacore::DeviceType::Switch, kSchemaDevice);
    }

    {
        // case: a JSON integer parameter that is not a whole int is HTTP 400 text in every dispatcher (#1476)
        // The query and form paths already refuse "0.5" and values outside int
        // (parse_int_value); the JSON-body path truncated 0.5 to 0 and wrapped
        // 2^32 to 0, so a stub that accepts 0 answered success.
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        constexpr int kNum = 9476;
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::TelescopeStub>(kNum)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::CameraStub>(kNum)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::SwitchStub>(kNum)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::FilterWheelStub>(kNum)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::FocuserStub>(kNum)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::CoverCalibratorStub>(kNum)));
        alpacahttp::Router router;
        struct Probe {
            const char* path;
            const char* param;
        };
        const Probe probes[] = {
            {"/api/v1/telescope/9476/slewsettletime", "SlewSettleTime"},
            {"/api/v1/camera/9476/gain", "Gain"},
            {"/api/v1/switch/9476/setswitchvalue", "Id"},
            {"/api/v1/filterwheel/9476/position", "Position"},
            {"/api/v1/focuser/9476/move", "Position"},
            {"/api/v1/covercalibrator/9476/calibratoron", "Brightness"},
        };
        std::vector<std::string> wrong;  // every probe that did not answer as expected, printed before the EXPECT
        for (const auto& probe : probes) {
            for (const char* bad : {"0.5", "4294967296", "-2147483649"}) {
                const std::string body = std::string("{\"") + probe.param + "\":" + bad + ",\"Value\":0}";
                const auto response = route_request(router, "PUT", probe.path, body);
                // Not a whole int: the request is not interpretable, HTTP 400 text (Alpaca API Reference s2.5).
                if (response.status_code() != 400 || response.get_header("Content-Type").find("text/plain") != 0 ||
                    response.body().find(probe.param) == std::string::npos) {
                    wrong.push_back(std::string(probe.path) + " " + probe.param + "=" + bad + " -> HTTP " +
                                    std::to_string(response.status_code()) + " body '" + response.body() + "'");
                }
            }
            // A whole number written as 0.0 stays accepted, as the query path accepts "0".
            const std::string whole = std::string("{\"") + probe.param + "\":0.0,\"Value\":0}";
            const auto ok_response = route_request(router, "PUT", probe.path, whole);
            const auto ok = nlohmann::json::parse(ok_response.body(), nullptr, false);
            if (ok_response.status_code() != 200 || ok.is_discarded() || ok.value("ErrorNumber", -1) != 0) {
                wrong.push_back(std::string(probe.path) + " " + probe.param + "=0.0 not accepted (ErrorNumber " +
                                std::to_string(ok.is_discarded() ? -2 : ok.value("ErrorNumber", -1)) + ")");
            }
        }
        for (const auto& line : wrong) std::cerr << "  " << line << "\n";
        EXPECT(wrong.empty());
        // A known method with its parameter missing is HTTP 400 text naming it (#1476).
        const auto missing = route_request(router, "PUT", "/api/v1/focuser/9476/move", "{\"ClientID\":1}");
        EXPECT(missing.status_code() == 400);
        EXPECT(missing.get_header("Content-Type").find("text/plain") == 0);
        EXPECT(missing.body().find("Missing parameter: Position") != std::string::npos);
        registry.unregister_device(alpacacore::DeviceType::Telescope, kNum);
        registry.unregister_device(alpacacore::DeviceType::Camera, kNum);
        registry.unregister_device(alpacacore::DeviceType::Switch, kNum);
        registry.unregister_device(alpacacore::DeviceType::FilterWheel, kNum);
        registry.unregister_device(alpacacore::DeviceType::Focuser, kNum);
        registry.unregister_device(alpacacore::DeviceType::CoverCalibrator, kNum);
    }
    {
        // case: a verb other than GET on an Alpaca-defined management path is 400 + text/plain (#1508)
        alpacahttp::Router router;
        for (const std::string path :
             {"/management/apiversions", "/management/v1/apiversions", "/management/v1/configureddevices",
              "/management/configureddevices", "/management/v1/description", "/management/description"}) {
            const auto get = route_request(router, "GET", path);
            EXPECT(get.status_code() == 200);
            for (const char* verb : {"PUT", "POST", "DELETE"}) {
                if (path.find("description") != std::string::npos && std::string(verb) != "DELETE") continue;
                const auto response = route_request(router, verb, path, "{}");
                EXPECT(response.status_code() == 400);
                EXPECT(response.get_header("Content-Type").find("text/plain") == 0);
                EXPECT(response.body().find("Unsupported HTTP method") == 0);
            }
        }
    }
    {
        // case: a description write the server cannot interpret is refused with 400 (#1476)
        // Missing body, bad JSON, no known property and an invalid field value
        // join the AllowedHosts and self-lockout refusals at 400 (ADR 0007).
        const ScopedCwd scratch;
        alpacahttp::Router router;
        const std::string path = "/management/v1/description";
        struct Bad {
            const char* body;
            int error;
        };
        const Bad cases[] = {
            {"", 0x402},
            {"not json", 0x401},
            {"{}", 0x402},
            {"{\"HostCheckEnabled\":\"maybe\"}", 0x401},
            {"{\"Location\":42}", 0x401},
            {"{\"ProfileName\":null}", 0x401},
            {"{\"profile_name\":[]}", 0x401},
        };
        for (const char* verb : {"PUT", "POST"}) {
            for (const auto& bad : cases) {
                const auto response = route_request(router, verb, path, bad.body);
                EXPECT(response.status_code() == 400);
                const auto json = nlohmann::json::parse(response.body(), nullptr, false);
                EXPECT(!json.is_discarded() && json.value("ErrorNumber", 0) == bad.error);
            }
        }
        EXPECT(route_request(router, "GET", path).status_code() == 200);
    }

    {
        // case: a missing or unparsable parameter is HTTP 400 text/plain naming it, never a driver call (#1476)
        // Alpaca API Reference s2.5: "All information required present?" NO -> 400. A parsed value
        // the device refuses (out of range) stays 200 + InvalidValue.
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        constexpr int kNum = 9477;
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::FocuserStub>(kNum)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::TelescopeStub>(kNum)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::CameraStub>(kNum)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::FilterWheelStub>(kNum)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::RotatorStub>(kNum)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::DomeStub>(kNum)));
        EXPECT(registry.register_device(std::make_shared<route_table_stubs::ObservingConditionsStub>(kNum)));
        alpacahttp::Router router;
        struct Bad {
            const char* verb;
            const char* path;
            const char* body;
            const char* param;
        };
        const Bad cases[] = {
            {"PUT", "/api/v1/focuser/9477/move", "", "Position"},                           // missing
            {"PUT", "/api/v1/focuser/9477/move", "{\"ClientID\":1}", "Position"},           // missing, JSON
            {"PUT", "/api/v1/focuser/9477/move?Position=abc", "", "Position"},              // query
            {"PUT", "/api/v1/focuser/9477/move", "Position=abc", "Position"},               // form
            {"PUT", "/api/v1/focuser/9477/move", "{\"Position\":\"abc\"}", "Position"},     // JSON string
            {"PUT", "/api/v1/focuser/9477/move", "{\"Position\":[1]}", "Position"},         // JSON wrong type
            {"PUT", "/api/v1/focuser/9477/move", "{\"Position\":0.5}", "Position"},         // not whole
            {"PUT", "/api/v1/focuser/9477/move", "{\"Position\":4294967296}", "Position"},  // outside int
            {"PUT", "/api/v1/focuser/9477/move?Position=4294967296", "", "Position"},       // outside int
            {"PUT", "/api/v1/focuser/9477/tempcomp?TempComp=maybe", "", "TempComp"},        // bool
            {"PUT", "/api/v1/telescope/9477/slewtocoordinatesasync?RightAscension=x&Declination=1", "",
             "RightAscension"},
            {"PUT", "/api/v1/telescope/9477/synctocoordinates?RightAscension=1", "", "Declination"},
            {"PUT", "/api/v1/camera/9477/gain?Gain=1.5", "", "Gain"},
            {"PUT", "/api/v1/camera/9477/startexposure?Duration=1", "", "Light"},
            {"PUT", "/api/v1/focuser/9477/connected?Connected=perhaps", "", "Connected"},
            {"PUT", "/api/v1/telescope/9477/utcdate", "{\"UTCDate\":5}", "UTCDate"},  // JSON wrong type
            {"PUT", "/api/v1/telescope/9477/utcdate", "{\"Value\":5}", "UTCDate"},    // JSON wrong type
            {"PUT", "/api/v1/filterwheel/9477/focusoffsets", "{\"FocusOffsets\":\"abc\"}", "FocusOffsets"},
            {"PUT", "/api/v1/filterwheel/9477/focusoffsets", "{\"FocusOffsets\":5}", "FocusOffsets"},
            {"PUT", "/api/v1/filterwheel/9477/focusoffsets", "{\"FocusOffsets\":[\"x\"]}", "FocusOffsets"},
            {"PUT", "/api/v1/filterwheel/9477/focusoffsets", "{\"FocusOffsets\":[0.5]}", "FocusOffsets"},
            {"PUT", "/api/v1/filterwheel/9477/focusoffsets", "{\"FocusOffsets\":[4294967296]}", "FocusOffsets"},
            {"PUT", "/api/v1/rotator/9477/moveabsolute", "Position=abc", "Position"},
            {"PUT", "/api/v1/dome/9477/slewtoazimuth", "Azimuth=abc", "Azimuth"},
            {"PUT", "/api/v1/observingconditions/9477/averageperiod", "AveragePeriod=abc", "AveragePeriod"},
            {"GET", "/api/v1/observingconditions/9477/sensordescription", "", "SensorName"},  // missing
        };
        std::vector<std::string> wrong;
        for (const auto& bad : cases) {
            const auto response = route_request(router, bad.verb, bad.path, bad.body);
            const bool text = response.get_header("Content-Type").find("text/plain") == 0;
            const bool json = !nlohmann::json::parse(response.body(), nullptr, false).is_discarded() &&
                              nlohmann::json::parse(response.body(), nullptr, false).is_object();
            if (response.status_code() != 400 || !text || json || response.body().empty() ||
                response.body().find(bad.param) == std::string::npos) {
                wrong.push_back(std::string(bad.path) + " [" + bad.body + "] -> HTTP " +
                                std::to_string(response.status_code()) + " type '" +
                                response.get_header("Content-Type") + "' body '" + response.body() + "'");
            }
        }
        for (const auto& line : wrong) std::cerr << "  " << line << "\n";
        EXPECT(wrong.empty());
        const auto offsets =
            route_request(router, "PUT", "/api/v1/filterwheel/9477/focusoffsets", "{\"FocusOffsets\":[1,2]}");
        EXPECT(offsets.status_code() == 200);
        EXPECT(nlohmann::json::parse(offsets.body(), nullptr, false).value("ErrorNumber", -1) == 0);
        // A numeric string element parses like any JSON integer parameter (json_int_value).
        const auto string_offsets =
            route_request(router, "PUT", "/api/v1/filterwheel/9477/focusoffsets", "{\"FocusOffsets\":[\"5\",2]}");
        EXPECT(string_offsets.status_code() == 200);
        EXPECT(nlohmann::json::parse(string_offsets.body(), nullptr, false).value("ErrorNumber", -1) == 0);
        const auto read_back = route_request(router, "GET", "/api/v1/filterwheel/9477/focusoffsets");
        EXPECT(nlohmann::json::parse(read_back.body(), nullptr, false).value("Value", nlohmann::json()) ==
               nlohmann::json::array({5, 2}));

        // A parsed value the driver range-checks stays 200 + InvalidValue (0x401).
        class RangeCheckedSwitch final : public route_table_stubs::SwitchStub {
        public:
            using SwitchStub::SwitchStub;
            int get_max_switch() const override { return 1; }
            bool get_can_async(int id) const override {
                if (id < 0 || id >= get_max_switch()) {
                    throw alpacacore::AlpacaException("Switch id out of range", alpacacore::AlpacaError::InvalidValue);
                }
                return true;
            }
        };
        EXPECT(registry.register_device(std::make_shared<RangeCheckedSwitch>(kNum)));
        const auto range = route_request(router, "GET", "/api/v1/switch/9477/canasync?Id=7");
        EXPECT(range.status_code() == 200);
        EXPECT(nlohmann::json::parse(range.body(), nullptr, false).value("ErrorNumber", -1) == 0x401);
        const auto in_range = route_request(router, "GET", "/api/v1/switch/9477/canasync?Id=0");
        EXPECT(in_range.status_code() == 200);
        EXPECT(nlohmann::json::parse(in_range.body(), nullptr, false).value("ErrorNumber", -1) == 0);
        const auto unparsable = route_request(router, "GET", "/api/v1/switch/9477/canasync?Id=seven");
        EXPECT(unparsable.status_code() == 400);
        registry.unregister_device(alpacacore::DeviceType::Switch, kNum);

        registry.unregister_device(alpacacore::DeviceType::Focuser, kNum);
        registry.unregister_device(alpacacore::DeviceType::Telescope, kNum);
        registry.unregister_device(alpacacore::DeviceType::Camera, kNum);
        registry.unregister_device(alpacacore::DeviceType::FilterWheel, kNum);
        registry.unregister_device(alpacacore::DeviceType::Rotator, kNum);
        registry.unregister_device(alpacacore::DeviceType::Dome, kNum);
        registry.unregister_device(alpacacore::DeviceType::ObservingConditions, kNum);
    }
    {
        // case: every Device API 400 is text/plain with a non-empty message, no JSON envelope (#1476)
        alpacahttp::Router router;
        struct Site {
            const char* verb;
            const char* path;
            const char* needle;
        };
        const Site sites[] = {
            {"GET", "/api/v1/widget/0/name", "device type"},               // unknown device type
            {"GET", "/api/v1/focuser/99999999999/name", "device number"},  // invalid device number
            {"GET", "/api/v1/focuser/0/nosuchmethod", "method"},           // unknown method
            {"PUT", "/api/v1/focuser/0/name", "name"},                     // wrong verb
            {"GET", "/api/v1/focuser/4242/name", "4242"},                  // unregistered device
        };
        std::vector<std::string> wrong;
        for (const auto& site : sites) {
            const auto response = route_request(router, site.verb, site.path, "");
            const auto parsed = nlohmann::json::parse(response.body(), nullptr, false);
            if (response.status_code() != 400 || response.get_header("Content-Type").find("text/plain") != 0 ||
                response.body().empty() || parsed.is_object() ||
                response.body().find(site.needle) == std::string::npos) {
                wrong.push_back(std::string(site.path) + " -> HTTP " + std::to_string(response.status_code()) +
                                " type '" + response.get_header("Content-Type") + "' body '" + response.body() + "'");
            }
        }
        // The over-long path is answered before routing, text/plain on every path.
        const auto long_path = route_request(router, "GET", "/" + std::string(70000, 'a'));
        if (long_path.status_code() != 400 || long_path.get_header("Content-Type").find("text/plain") != 0 ||
            nlohmann::json::parse(long_path.body(), nullptr, false).is_object()) {
            wrong.push_back("over-long path -> HTTP " + std::to_string(long_path.status_code()) + " type '" +
                            long_path.get_header("Content-Type") + "'");
        }
        for (const auto& line : wrong) std::cerr << "  " << line << "\n";
        EXPECT(wrong.empty());
    }
    {
        // case: the AlpacaBridge-only management 400s keep the JSON envelope (#1476, ADR 0007)
        const ScopedCwd scratch;
        alpacahttp::Router router;
        const auto description = route_request(router, "PUT", "/management/v1/description", "{}");
        EXPECT(description.status_code() == 400);
        EXPECT(description.get_header("Content-Type").find("application/json") == 0);
        EXPECT(nlohmann::json::parse(description.body(), nullptr, false).is_object());
        // The update/channel 400 envelope is pinned in "update channel route", whose router has a manager.
    }

    std::cout << "All routing tests passed!\n";
    return 0;
}

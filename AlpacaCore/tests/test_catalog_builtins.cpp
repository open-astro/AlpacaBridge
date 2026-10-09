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

// The built-in descriptors (open-astro#664, Part D): what
// register_builtin_schemas() and register_builtin_factories() put into a
// catalog. The schema half compiles in every build, so the first two cases
// run vendors-off too; the factory-selection cases need the Astroasis driver.
// Fake-only: Astroasis has no fake, so factory selection is observed through
// the connect refusal each factory produces without hardware (the fixed-path
// factory names the node it failed to open; the by-index factory reports the
// USB scan). Neither opens a real focuser: the path does not exist and the
// index is far beyond any bus.

#include <alpacacore/catalog/builtin_catalog.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_io.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "catch2_compat.h"

using namespace alpacacore;
using namespace alpacacore::catalog;

namespace {

const DeviceKey kAstroasisKey{"astroasis", DeviceType::Focuser};

const DescriptorView* find_view(const std::vector<DescriptorView>& views, const DeviceKey& key) {
    for (const DescriptorView& v : views) {
        if (v.key == key) return &v;
    }
    return nullptr;
}

const FieldRef* find_field(std::span<const FieldRef> fields, std::string_view key) {
    for (const FieldRef& f : fields) {
        if (key == f.key) return &f;
    }
    return nullptr;
}

DeviceCatalog builtin_catalog() {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    register_builtin_factories(catalog);
    return catalog;
}

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the Astroasis focuser in every build",
          "[catalog][astroasis][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kAstroasisKey);
    REQUIRE(v != nullptr);
    CHECK(v->display_name == "Astroasis Oasis Focuser");
    CHECK(v->build_option == "ALPACACORE_ENABLE_ASTROASIS");
    CHECK_FALSE(v->available);  // schemas only: no factory has been registered yet
    REQUIRE(v->fields.size() == 2);
    CHECK(std::string_view(v->fields[0].key) == "hidPath");
    CHECK(std::string_view(v->fields[1].key) == "focuserIndex");

    const FieldRef* hid = find_field(v->fields, "hidPath");
    REQUIRE(hid != nullptr);
    CHECK(hid->kind == FieldRef::Kind::String);
    CHECK(hid->role == Role::PortPath);
    CHECK_FALSE(hid->required);
    CHECK_FALSE(hid->applies_when.has_value());
    CHECK(hid->allowed_values.empty());
    CHECK_FALSE(hid->min.has_value());
    CHECK_FALSE(hid->max.has_value());
    REQUIRE(std::holds_alternative<std::string>(hid->default_value));
    CHECK(std::get<std::string>(hid->default_value).empty());

    const FieldRef* index = find_field(v->fields, "focuserIndex");
    REQUIRE(index != nullptr);
    CHECK(index->kind == FieldRef::Kind::Int);
    CHECK(index->role == Role::EnumerationIndex);
    CHECK_FALSE(index->required);
    CHECK_FALSE(index->applies_when.has_value());
    CHECK(index->allowed_values.empty());
    CHECK_FALSE(index->min.has_value());  // ALP-271: no range declared, see astroasis_fields.h
    CHECK_FALSE(index->max.has_value());  // F2 pins no range for focuserIndex either
    REQUIRE(std::holds_alternative<std::int64_t>(index->default_value));
    CHECK(std::get<std::int64_t>(index->default_value) == 0);

    // No rule on focuserIndex: a negative index passes through unchanged from
    // both sources -- the arm it replaces never validated the index either.
    DeviceConfig negative;
    negative.set("focuserIndex", std::int64_t{-1});
    const auto negative_api = catalog.normalize(kAstroasisKey, negative, Source::Api);
    CHECK_FALSE(negative_api.rejection.has_value());
    const auto negative_persisted = catalog.normalize(kAstroasisKey, negative, Source::Persisted);
    CHECK(negative_persisted.warnings.empty());
    REQUIRE(negative_persisted.config.has("focuserIndex"));
    const ConfigValue* negative_value = negative_persisted.config.find_value("focuserIndex");
    REQUIRE(negative_value != nullptr);
    REQUIRE(std::holds_alternative<std::int64_t>(*negative_value));
    CHECK(std::get<std::int64_t>(*negative_value) == -1);

    // Both keys accepted together, and an empty config normalizes to itself.
    DeviceConfig both;
    both.set("hidPath", std::string{"/dev/hidraw3"});
    both.set("focuserIndex", std::int64_t{2});
    CHECK_FALSE(catalog.normalize(kAstroasisKey, both, Source::Api).rejection.has_value());
    CHECK_FALSE(catalog.normalize(kAstroasisKey, DeviceConfig{}, Source::Api).rejection.has_value());

    // Sanitize keeps both (neither is a secret) and drops an undeclared key.
    both.set("junk", true);
    const DeviceConfig sanitized = catalog.sanitize(kAstroasisKey, both);
    CHECK(sanitized.has("hidPath"));
    CHECK(sanitized.has("focuserIndex"));
    CHECK_FALSE(sanitized.has("junk"));
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the Astroasis focuser available only when built",
          "[catalog][astroasis][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kAstroasisKey);
    REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_ASTROASIS
    CHECK(v->available);
#else
    CHECK_FALSE(v->available);
    // The catalog's own refusal names the build option; the router turns it
    // into the "<Vendor> support not enabled" text F2 pins (open-astro#664 Part C).
    CHECK_THROWS_AS(catalog.create(kAstroasisKey, DeviceConfig{}, 0), std::runtime_error);
    try {
        (void)catalog.create(kAstroasisKey, DeviceConfig{}, 0);
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_ASTROASIS") != std::string::npos);
    }
#endif
}

#ifdef ALPACACORE_ENABLE_ASTROASIS

namespace {

// The connect refusal a driver built from `config` produces with no hardware.
std::string connect_refusal(const DeviceCatalog& catalog, const DeviceConfig& config, int device_number) {
    auto driver = catalog.create(kAstroasisKey, config, device_number);
    REQUIRE(driver != nullptr);
    CHECK(driver->get_device_type() == DeviceType::Focuser);
    CHECK(driver->get_device_number() == device_number);
    CHECK_FALSE(driver->get_connected());
    try {
        driver->set_connected(true);
    } catch (const AlpacaException& e) {
        CHECK_FALSE(driver->get_connected());
        return e.what();
    }
    FAIL("set_connected(true) must throw with no focuser attached");
    return "";
}

}  // namespace

TEST_CASE("Builtin catalog - the Astroasis factory picks the hidPath factory when hidPath is set",
          "[catalog][astroasis][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const std::string path = "/dev/hidraw-alp255-none";
    DeviceConfig config;
    config.set("hidPath", path);
    config.set("focuserIndex", std::int64_t{1000000});  // ignored: hidPath wins, as the arm did it
    const std::string refusal = connect_refusal(catalog, config, 7);
    INFO(refusal);
    CHECK(refusal.find(path) != std::string::npos);
    CHECK(refusal.find("detected on the USB bus") == std::string::npos);
    CHECK(refusal.find("out of range") == std::string::npos);
}

TEST_CASE("Builtin catalog - the Astroasis factory scans by index when hidPath is empty or absent",
          "[catalog][astroasis][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    for (const bool with_empty_path : {true, false}) {
        DeviceConfig config;
        if (with_empty_path) config.set("hidPath", std::string{});
        config.set("focuserIndex", std::int64_t{1000000});
        const std::string refusal = connect_refusal(catalog, config, 8);
        INFO(refusal);
        // No hardware: the scan finds nothing. A bus with a real focuser on it
        // answers the out-of-range index instead; both come from the scan and
        // neither opens a device.
        CHECK((refusal.find("No Astroasis Oasis Focuser detected on the USB bus") != std::string::npos ||
               refusal.find("Focuser index 1000000 out of range") != std::string::npos));
        CHECK(refusal.find("hidraw") == std::string::npos);
    }
}

#endif  // ALPACACORE_ENABLE_ASTROASIS

// ---------------------------------------------------------------------------
// WeeWX (open-astro#731): the second built-in descriptor, same split as
// Astroasis. The schema declares no required/min/max rule: the three refusals
// of the router arm it replaces ("... requires weewxUrl", "... must be greater
// than 0") come from the factory, so a persisted entry that breaks one is still
// not registered, as before the move. The factory also refuses a value above
// the int range the arm read both numbers in ("... must be at most 2147483647").

namespace {

const DeviceKey kWeeWxKey{"weewx", DeviceType::ObservingConditions};

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the WeeWX observing conditions in every build",
          "[catalog][weewx][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kWeeWxKey);
    REQUIRE(v != nullptr);
    // The router's not-enabled and registered texts use the first word.
    CHECK(v->display_name.substr(0, 6) == "WeeWX ");
    CHECK(v->build_option == "ALPACACORE_ENABLE_WEEWX");
    CHECK_FALSE(v->available);  // schemas only: no factory has been registered yet
    REQUIRE(v->fields.size() == 3);
    CHECK(std::string_view(v->fields[0].key) == "weewxUrl");
    CHECK(std::string_view(v->fields[1].key) == "pollIntervalSeconds");
    CHECK(std::string_view(v->fields[2].key) == "timeoutMs");

    for (const FieldRef& f : v->fields) {
        INFO(f.key);
        CHECK(f.role == Role::Plain);
        CHECK_FALSE(f.required);
        CHECK_FALSE(f.applies_when.has_value());
        CHECK(f.allowed_values.empty());
        CHECK_FALSE(f.min.has_value());
        CHECK_FALSE(f.max.has_value());
    }
    const FieldRef* url = find_field(v->fields, "weewxUrl");
    REQUIRE(url != nullptr);
    CHECK(url->kind == FieldRef::Kind::String);
    REQUIRE(std::holds_alternative<std::string>(url->default_value));
    CHECK(std::get<std::string>(url->default_value).empty());
    const FieldRef* poll = find_field(v->fields, "pollIntervalSeconds");
    REQUIRE(poll != nullptr);
    CHECK(poll->kind == FieldRef::Kind::Int);
    REQUIRE(std::holds_alternative<std::int64_t>(poll->default_value));
    CHECK(std::get<std::int64_t>(poll->default_value) == 900);
    const FieldRef* timeout = find_field(v->fields, "timeoutMs");
    REQUIRE(timeout != nullptr);
    CHECK(timeout->kind == FieldRef::Kind::Int);
    REQUIRE(std::holds_alternative<std::int64_t>(timeout->default_value));
    CHECK(std::get<std::int64_t>(timeout->default_value) == 5000);

    // No rule in the schema: a missing URL and zero values pass normalize
    // unchanged from both sources (the factory refuses them instead).
    DeviceConfig zeros;
    zeros.set("pollIntervalSeconds", std::int64_t{0});
    zeros.set("timeoutMs", std::int64_t{0});
    CHECK_FALSE(catalog.normalize(kWeeWxKey, zeros, Source::Api).rejection.has_value());
    const auto zeros_persisted = catalog.normalize(kWeeWxKey, zeros, Source::Persisted);
    CHECK(zeros_persisted.warnings.empty());
    CHECK(zeros_persisted.config.has("pollIntervalSeconds"));
    CHECK(zeros_persisted.config.has("timeoutMs"));

    // Sanitize keeps all three (none is a secret) and drops an undeclared key.
    DeviceConfig all;
    all.set("weewxUrl", std::string{"http://weewx.test:8998/current.json"});
    all.set("pollIntervalSeconds", std::int64_t{300});
    all.set("timeoutMs", std::int64_t{2500});
    all.set("cameraIndex", std::int64_t{1});
    const DeviceConfig sanitized = catalog.sanitize(kWeeWxKey, all);
    CHECK(sanitized.has("weewxUrl"));
    CHECK(sanitized.has("pollIntervalSeconds"));
    CHECK(sanitized.has("timeoutMs"));
    CHECK_FALSE(sanitized.has("cameraIndex"));
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the WeeWX observing conditions available only when built",
          "[catalog][weewx][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kWeeWxKey);
    REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_WEEWX
    CHECK(v->available);
#else
    CHECK_FALSE(v->available);
    CHECK_THROWS_AS(catalog.create(kWeeWxKey, DeviceConfig{}, 0), std::runtime_error);
    try {
        (void)catalog.create(kWeeWxKey, DeviceConfig{}, 0);
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_WEEWX") != std::string::npos);
    }
#endif
}

// ---------------------------------------------------------------------------
// SVBONY camera: schema in every build, factory only when built.

namespace {

const DeviceKey kSvbonyKey{"svbony", DeviceType::Camera};

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the SVBONY camera in every build",
          "[catalog][svbony][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kSvbonyKey);
    REQUIRE(v != nullptr);
    CHECK(v->display_name.substr(0, 7) == "SVBONY ");
    CHECK(v->build_option == "ALPACACORE_ENABLE_SVBONY");
    CHECK_FALSE(v->available);
    REQUIRE(v->fields.size() == 1);
    const FieldRef& index = v->fields[0];
    CHECK(std::string_view(index.key) == "cameraIndex");
    CHECK(index.kind == FieldRef::Kind::Int);
    CHECK(index.role == Role::EnumerationIndex);
    CHECK_FALSE(index.required);
    REQUIRE(index.min.has_value());
    CHECK(*index.min == 0.0);
    REQUIRE(index.max.has_value());
    CHECK(*index.max == static_cast<double>(std::numeric_limits<int>::max()));
    REQUIRE(std::holds_alternative<std::int64_t>(index.default_value));
    CHECK(std::get<std::int64_t>(index.default_value) == 0);

    // A negative or above-INT_MAX index is refused at save time, before the
    // factory's static_cast<int> could wrap it.
    for (const std::int64_t bad : {std::int64_t{-1}, std::int64_t{std::numeric_limits<int>::max()} + 1}) {
        DeviceConfig config;
        config.set("cameraIndex", bad);
        const auto api = catalog.normalize(kSvbonyKey, config, Source::Api);
        CHECK(api.rejection.has_value());
    }
    DeviceConfig edge;
    edge.set("cameraIndex", std::int64_t{std::numeric_limits<int>::max()});
    CHECK_FALSE(catalog.normalize(kSvbonyKey, edge, Source::Api).rejection.has_value());

    // Sanitize keeps cameraIndex and drops undeclared keys (cameraId).
    DeviceConfig all;
    all.set("cameraIndex", std::int64_t{2});
    all.set("cameraId", std::string{"x"});
    const DeviceConfig sanitized = catalog.sanitize(kSvbonyKey, all);
    CHECK(sanitized.has("cameraIndex"));
    CHECK_FALSE(sanitized.has("cameraId"));
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the SVBONY camera available only when built",
          "[catalog][svbony][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kSvbonyKey);
    REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_SVBONY
    CHECK(v->available);
#else
    CHECK_FALSE(v->available);
    CHECK_THROWS_AS(catalog.create(kSvbonyKey, DeviceConfig{}, 0), std::runtime_error);
    try {
        (void)catalog.create(kSvbonyKey, DeviceConfig{}, 0);
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_SVBONY") != std::string::npos);
    }
#endif
}

// ---------------------------------------------------------------------------
// gphoto camera: schema in every build, factory only when built.

namespace {

const DeviceKey kGphotoKey{"gphoto", DeviceType::Camera};

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the gphoto camera in every build",
          "[catalog][gphoto][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kGphotoKey);
    REQUIRE(v != nullptr);
    // The vendor-off refusal text takes the label from the first word.
    CHECK(v->display_name.substr(0, 7) == "gphoto ");
    CHECK(v->build_option == "ALPACACORE_ENABLE_GPHOTO");
    CHECK_FALSE(v->available);
    REQUIRE(v->fields.size() == 1);
    const FieldRef& index = v->fields[0];
    CHECK(std::string_view(index.key) == "cameraIndex");
    CHECK(index.kind == FieldRef::Kind::Int);
    CHECK(index.role == Role::EnumerationIndex);
    CHECK_FALSE(index.required);
    CHECK_FALSE(index.min.has_value());
    CHECK_FALSE(index.max.has_value());
    REQUIRE(std::holds_alternative<std::int64_t>(index.default_value));
    CHECK(std::get<std::int64_t>(index.default_value) == 0);

    DeviceConfig all;
    all.set("cameraIndex", std::int64_t{2});
    all.set("cameraId", std::string{"x"});
    const DeviceConfig sanitized = catalog.sanitize(kGphotoKey, all);
    CHECK(sanitized.has("cameraIndex"));
    CHECK_FALSE(sanitized.has("cameraId"));
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the gphoto camera available only when built",
          "[catalog][gphoto][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kGphotoKey);
    REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_GPHOTO
    CHECK(v->available);
#else
    CHECK_FALSE(v->available);
    CHECK_THROWS_AS(catalog.create(kGphotoKey, DeviceConfig{}, 0), std::runtime_error);
    try {
        (void)catalog.create(kGphotoKey, DeviceConfig{}, 0);
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_GPHOTO") != std::string::npos);
    }
#endif
}

// ---------------------------------------------------------------------------
// Player One camera, Phoenix filter wheel and thermal switch: schemas in every
// build, factories only when built.

namespace {

const DeviceKey kPlayerOneCameraKey{"playerone", DeviceType::Camera};
const DeviceKey kPlayerOneWheelKey{"playerone", DeviceType::FilterWheel};
const DeviceKey kPlayerOneSwitchKey{"playerone", DeviceType::Switch};

void check_index_field(const FieldRef& f, std::string_view key) {
    CHECK(std::string_view(f.key) == key);
    CHECK(f.kind == FieldRef::Kind::Int);
    CHECK(f.role == Role::EnumerationIndex);
    CHECK_FALSE(f.required);
    CHECK_FALSE(f.min.has_value());
    CHECK_FALSE(f.max.has_value());
    REQUIRE(std::holds_alternative<std::int64_t>(f.default_value));
    CHECK(std::get<std::int64_t>(f.default_value) == 0);
}

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the Player One devices in every build",
          "[catalog][playerone][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    for (const DeviceKey& key : {kPlayerOneCameraKey, kPlayerOneWheelKey, kPlayerOneSwitchKey}) {
        const DescriptorView* v = find_view(views, key);
        REQUIRE(v != nullptr);
        // Two words: the router's first-word default would say "Player".
        CHECK(v->vendor_label == "Player One");
        CHECK(v->build_option == "ALPACACORE_ENABLE_PLAYERONE");
        CHECK_FALSE(v->available);
    }

    const DescriptorView* camera = find_view(views, kPlayerOneCameraKey);
    REQUIRE(camera->fields.size() == 1);
    check_index_field(camera->fields[0], "cameraIndex");
    // The thermal switch binds to a camera by its index.
    const DescriptorView* sw = find_view(views, kPlayerOneSwitchKey);
    REQUIRE(sw->fields.size() == 1);
    check_index_field(sw->fields[0], "cameraIndex");

    const DescriptorView* wheel = find_view(views, kPlayerOneWheelKey);
    REQUIRE(wheel->fields.size() == 2);
    check_index_field(wheel->fields[0], "filterwheelIndex");
    const FieldRef& names = wheel->fields[1];
    CHECK(std::string_view(names.key) == "filterNames");
    CHECK(names.kind == FieldRef::Kind::StringList);
    CHECK_FALSE(names.required);
    REQUIRE(std::holds_alternative<std::vector<std::string>>(names.default_value));
    CHECK(std::get<std::vector<std::string>>(names.default_value).empty());

    // Sanitize keeps each type's own fields, as the deleted router branch did.
    DeviceConfig all;
    all.set("cameraIndex", std::int64_t{3});
    all.set("filterwheelIndex", std::int64_t{2});
    all.set("filterNames", std::vector<std::string>{"L", "R"});
    all.set("switchType", std::string{"x"});
    const DeviceConfig cam_cfg = catalog.sanitize(kPlayerOneCameraKey, all);
    CHECK(cam_cfg.has("cameraIndex"));
    CHECK_FALSE(cam_cfg.has("filterwheelIndex"));
    CHECK_FALSE(cam_cfg.has("filterNames"));
    const DeviceConfig sw_cfg = catalog.sanitize(kPlayerOneSwitchKey, all);
    CHECK(sw_cfg.has("cameraIndex"));
    CHECK_FALSE(sw_cfg.has("switchType"));
    const DeviceConfig wheel_cfg = catalog.sanitize(kPlayerOneWheelKey, all);
    CHECK(wheel_cfg.has("filterwheelIndex"));
    CHECK_FALSE(wheel_cfg.has("cameraIndex"));
    const ConfigValue* kept = wheel_cfg.find_value("filterNames");
    REQUIRE(kept != nullptr);
    REQUIRE(std::holds_alternative<std::vector<std::string>>(*kept));
    CHECK(std::get<std::vector<std::string>>(*kept) == std::vector<std::string>{"L", "R"});
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the Player One devices available only when built",
          "[catalog][playerone][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    for (const DeviceKey& key : {kPlayerOneCameraKey, kPlayerOneWheelKey, kPlayerOneSwitchKey}) {
        const DescriptorView* v = find_view(views, key);
        REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_PLAYERONE
        CHECK(v->available);
#else
        CHECK_FALSE(v->available);
        try {
            (void)catalog.create(key, DeviceConfig{}, 0);
            FAIL("create() without a Player One factory must throw");
        } catch (const std::runtime_error& e) {
            CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_PLAYERONE") != std::string::npos);
        }
#endif
    }
#ifdef ALPACACORE_ENABLE_PLAYERONE
    // filterNames reaches the driver, as the deleted arm's set_names() did.
    DeviceConfig cfg;
    cfg.set("filterNames", std::vector<std::string>{"L", "R", "G"});
    auto driver = catalog.create(kPlayerOneWheelKey, cfg, 0);
    auto* wheel = dynamic_cast<FilterWheelDriver*>(driver.get());
    REQUIRE(wheel != nullptr);
    CHECK(wheel->get_names() == std::vector<std::string>{"L", "R", "G"});
#endif
}

#ifdef ALPACACORE_ENABLE_WEEWX

namespace {

// A loopback HTTP endpoint the WeeWX driver can be pointed at. Answering, it
// serves a minimal current-conditions payload to every request; silent, it
// accepts each connection and never answers, so the client's own timeout ends
// the request. `requests()` counts connections accepted.
class LoopbackWeeWx {
public:
    explicit LoopbackWeeWx(bool answer) : answer_(answer) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        // Non-blocking, so a connection reset between poll() and accept()
        // makes accept() fail with EAGAIN instead of blocking serve() and the
        // destructor's join. An accepted fd does not inherit the flag on Linux.
        REQUIRE(alpacacore::util::set_nonblocking(listen_fd_));
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listen_fd_, 8) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~LoopbackWeeWx() {
        stop_ = true;
        thread_.join();
        for (const int fd : held_) ::close(fd);
        ::close(listen_fd_);
    }
    LoopbackWeeWx(const LoopbackWeeWx&) = delete;
    LoopbackWeeWx& operator=(const LoopbackWeeWx&) = delete;

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/current.json"; }
    int requests() const { return requests_.load(); }

private:
    void serve() {
        while (!stop_) {
            pollfd pfd{listen_fd_, POLLIN, 0};
            if (::poll(&pfd, 1, 20) <= 0) continue;
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) continue;
            ++requests_;
            if (!answer_) {
                held_.push_back(fd);
                continue;
            }
            std::string request;
            char buf[1024];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                request.append(buf, static_cast<std::size_t>(n));
            }
            const std::string body = R"({"lcd_datasheet":{"current":{"outTemp":{"value":50.0}}}})";
            const std::string reply =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            (void)::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
            ::close(fd);
        }
    }

    bool answer_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> requests_{0};
    std::vector<int> held_;  // serve() thread only, until the join
    std::thread thread_;
};

std::string create_refusal(const DeviceCatalog& catalog, const DeviceConfig& config) {
    try {
        (void)catalog.create(kWeeWxKey, config, 0);
    } catch (const AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::InvalidValue);
        return e.what();
    }
    FAIL("the WeeWX factory must refuse this config");
    return "";
}

}  // namespace

TEST_CASE("Builtin catalog - the WeeWX factory refuses what the router arm refused, with its text",
          "[catalog][weewx][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    DeviceConfig no_url;
    CHECK(create_refusal(catalog, no_url) == "WeeWX observing conditions requires weewxUrl");
    DeviceConfig empty_url;
    empty_url.set("weewxUrl", std::string{});
    CHECK(create_refusal(catalog, empty_url) == "WeeWX observing conditions requires weewxUrl");

    DeviceConfig poll_zero;
    poll_zero.set("weewxUrl", std::string{"http://weewx.test:8998/current.json"});
    poll_zero.set("pollIntervalSeconds", std::int64_t{0});
    CHECK(create_refusal(catalog, poll_zero) == "pollIntervalSeconds must be greater than 0");

    DeviceConfig timeout_negative;
    timeout_negative.set("weewxUrl", std::string{"http://weewx.test:8998/current.json"});
    timeout_negative.set("timeoutMs", std::int64_t{-1});
    CHECK(create_refusal(catalog, timeout_negative) == "timeoutMs must be greater than 0");
}

TEST_CASE("Builtin catalog - the WeeWX factory refuses an interval or timeout above the int range",
          "[catalog][weewx][unit]") {
    // The deleted arm read both fields as int. The schema's Int is 64-bit, and
    // 1e10 s converted to the nanoseconds wait_for() uses overflows, so the poll
    // thread waited 0 ms and fetched back to back.
    const DeviceCatalog catalog = builtin_catalog();
    DeviceConfig poll_huge;
    poll_huge.set("weewxUrl", std::string{"http://weewx.test:8998/current.json"});
    poll_huge.set("pollIntervalSeconds", std::int64_t{10000000000});
    CHECK(create_refusal(catalog, poll_huge) == "pollIntervalSeconds must be at most 2147483647");

    DeviceConfig timeout_huge;
    timeout_huge.set("weewxUrl", std::string{"http://weewx.test:8998/current.json"});
    timeout_huge.set("timeoutMs", std::int64_t{10000000000});
    CHECK(create_refusal(catalog, timeout_huge) == "timeoutMs must be at most 2147483647");
}

TEST_CASE("Builtin catalog - the WeeWX factory passes weewxUrl and timeoutMs through", "[catalog][weewx][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const LoopbackWeeWx silent(false);
    DeviceConfig config;
    config.set("weewxUrl", silent.url());
    config.set("timeoutMs", std::int64_t{300});
    auto driver = catalog.create(kWeeWxKey, config, 5);
    REQUIRE(driver != nullptr);
    CHECK(driver->get_device_type() == DeviceType::ObservingConditions);
    CHECK(driver->get_device_number() == 5);

    // The connect reaches this endpoint (the URL) and gives up after about
    // 300 ms (the timeout); the 5000 ms default would still be waiting.
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(driver->set_connected(true), AlpacaException);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // serve() counts after its own poll() wakes, which a starved runner can
    // schedule after the client has already given up.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (silent.requests() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(silent.requests() >= 1);
    CHECK(elapsed < std::chrono::milliseconds(3000));
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Builtin catalog - the WeeWX factory passes pollIntervalSeconds through", "[catalog][weewx][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const LoopbackWeeWx server(true);
    DeviceConfig config;
    config.set("weewxUrl", server.url());
    config.set("pollIntervalSeconds", std::int64_t{1});
    auto driver = catalog.create(kWeeWxKey, config, 6);
    REQUIRE(driver != nullptr);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // One fetch at connect, one as the poll thread starts, then one per
    // interval: a third request within 5 s means a 1 s interval, where the
    // 900 s default would leave the count at two.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (server.requests() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(server.requests() >= 3);
    driver->set_connected(false);
}

#endif  // ALPACACORE_ENABLE_WEEWX

// ---------------------------------------------------------------------------
// Sky-Watcher direct (motor controller) telescope (open-astro#744). The first
// telescope descriptor and the first with source-dependent cross-field rules:
// Schema::normalize rejects a config from the API and warns about (or leaves
// alone) a saved one, exactly as the router arm it replaces did (#274, #380,
// #398, #508). The schema cases run in every build; the factory cases need the
// driver and construct without a scan (#659/#660): a nonexistent serial node
// refuses the connect, an auto config is only constructed, never connected.
// ---------------------------------------------------------------------------

namespace {

const DeviceKey kSkyWatcherKey{"skywatcher", DeviceType::Telescope};

// Rule 4 (#274): the arm's kMissingSite text, byte for byte.
const std::string kSkyWatcherMissingSite =
    "Site latitude and longitude are required for the Sky-Watcher direct driver: this mount stores no site of its "
    "own, and tracking direction, guide sign and pier side are all hemisphere-dependent";

std::string string_at(const DeviceConfig& config, std::string_view key) {
    const ConfigValue* v = config.find_value(key);
    REQUIRE(v != nullptr);
    REQUIRE(std::holds_alternative<std::string>(*v));
    return std::get<std::string>(*v);
}

// Scalar defaults compared by alternative and value (ConfigValue has no
// operator==: a DeviceConfig record list is not comparable).
bool same_scalar(const ConfigValue& a, const ConfigValue& b) {
    if (a.index() != b.index()) return false;
    if (const auto* s = std::get_if<std::string>(&a)) return *s == std::get<std::string>(b);
    if (const auto* i = std::get_if<std::int64_t>(&a)) return *i == std::get<std::int64_t>(b);
    if (const auto* d = std::get_if<double>(&a)) return *d == std::get<double>(b);
    if (const auto* f = std::get_if<bool>(&a)) return *f == std::get<bool>(b);
    return false;
}

bool any_contains(const std::vector<std::string>& messages, std::string_view fragment) {
    for (const std::string& m : messages) {
        if (m.find(fragment) != std::string::npos) return true;
    }
    return false;
}

// A serial config with a site: the smallest config every rule accepts.
DeviceConfig skywatcher_serial_with_site() {
    DeviceConfig config;
    config.set("connectionType", std::string{"serial"});
    config.set("portPath", std::string{"/dev/ttyUSB8"});
    config.set("baudRate", std::int64_t{9600});
    config.set("siteLatitude", 39.7392);
    config.set("siteLongitude", -104.9903);
    return config;
}

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the SkyWatcher telescope in every build",
          "[catalog][skywatcher][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kSkyWatcherKey);
    REQUIRE(v != nullptr);
    // Rule 1: the first word is exactly "SkyWatcher" so vendor_label() keeps
    // "SkyWatcher support not enabled" and "Registered SkyWatcher telescope".
    CHECK(v->display_name == "SkyWatcher direct (motor controller)");
    CHECK(v->build_option == "ALPACACORE_ENABLE_SKYWATCHER");
    CHECK_FALSE(v->available);  // schemas only: no factory has been registered yet

    // Rule 3: the twelve fields the arm read, in this order, with its defaults,
    // then the two motion limits (#436).
    const std::vector<std::string_view> expected_keys{"connectionType",
                                                      "mountIndex",
                                                      "portPath",
                                                      "baudRate",
                                                      "host",
                                                      "udpPort",
                                                      "responseTimeoutMs",
                                                      "siteLatitude",
                                                      "siteLongitude",
                                                      "siteElevation",
                                                      "apertureDiameter",
                                                      "focalLength",
                                                      "minAltitudeDeg",
                                                      "meridianLimitMinutes",
                                                      "decAxisSense"};
    REQUIRE(v->fields.size() == expected_keys.size());
    for (std::size_t i = 0; i < expected_keys.size(); ++i) {
        CHECK(std::string_view(v->fields[i].key) == expected_keys[i]);
    }

    struct Expect {
        std::string_view key;
        FieldRef::Kind kind;
        Role role;
        std::optional<std::string_view> applies_to;  // connectionType value, when applies_when is set
        ConfigValue default_value;
        std::optional<double> min;
        std::optional<double> max;
    };
    const std::vector<Expect> expectations{
        {"connectionType", FieldRef::Kind::String, Role::Discriminator, std::nullopt, std::string{"auto"}, {}, {}},
        {"mountIndex", FieldRef::Kind::Int, Role::EnumerationIndex, std::nullopt, std::int64_t{0}, {}, {}},
        {"portPath", FieldRef::Kind::String, Role::PortPath, "serial", std::string{}, {}, {}},
        {"baudRate", FieldRef::Kind::Int, Role::Plain, "serial", std::int64_t{9600}, {}, {}},
        {"host", FieldRef::Kind::String, Role::Host, "network", std::string{}, {}, {}},
        {"udpPort", FieldRef::Kind::Int, Role::Plain, "network", std::int64_t{11880}, {}, {}},
        {"responseTimeoutMs", FieldRef::Kind::Int, Role::Plain, std::nullopt, std::int64_t{1000}, {}, {}},
        {"siteLatitude", FieldRef::Kind::Double, Role::Plain, std::nullopt, 0.0, -90.0, 90.0},
        {"siteLongitude", FieldRef::Kind::Double, Role::Plain, std::nullopt, 0.0, -180.0, 180.0},
        {"siteElevation", FieldRef::Kind::Double, Role::Plain, std::nullopt, 0.0, {}, {}},
        {"apertureDiameter", FieldRef::Kind::Double, Role::Plain, std::nullopt, 0.0, {}, {}},
        {"focalLength", FieldRef::Kind::Double, Role::Plain, std::nullopt, 0.0, {}, {}},
        {"minAltitudeDeg", FieldRef::Kind::Double, Role::Plain, std::nullopt, 0.0, -90.0, 90.0},
        {"meridianLimitMinutes", FieldRef::Kind::Double, Role::Plain, std::nullopt, 0.0, 0.0, 360.0},
        {"decAxisSense", FieldRef::Kind::String, Role::Plain, std::nullopt, std::string{"auto"}, {}, {}},
    };
    for (const Expect& e : expectations) {
        INFO(e.key);
        const FieldRef* f = find_field(v->fields, e.key);
        REQUIRE(f != nullptr);
        CHECK(f->kind == e.kind);
        CHECK(f->role == e.role);
        CHECK_FALSE(f->required);          // rule 3: nothing is required; rule 4 is a cross-field rule
        CHECK(f->allowed_values.empty());  // rule 5: connectionType has NO allowed_values
        CHECK(same_scalar(f->default_value, e.default_value));
        CHECK(f->min == e.min);
        CHECK(f->max == e.max);
        if (e.applies_to) {
            REQUIRE(f->applies_when.has_value());
            CHECK(std::string_view(f->applies_when->discriminator_key) == "connectionType");
            CHECK(std::string_view(f->applies_when->value) == *e.applies_to);
        } else {
            CHECK_FALSE(f->applies_when.has_value());
        }
    }
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the SkyWatcher telescope available only when built",
          "[catalog][skywatcher][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kSkyWatcherKey);
    REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_SKYWATCHER
    CHECK(v->available);
#else
    CHECK_FALSE(v->available);
    CHECK_THROWS_AS(catalog.create(kSkyWatcherKey, skywatcher_serial_with_site(), 0), std::runtime_error);
    try {
        (void)catalog.create(kSkyWatcherKey, skywatcher_serial_with_site(), 0);
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_SKYWATCHER") != std::string::npos);
    }
#endif
}

TEST_CASE("Builtin catalog - SkyWatcher normalize requires a site from the API and leaves a saved config alone (#274)",
          "[catalog][skywatcher][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    // Rule 4, Api: neither, latitude only, longitude only -> the exact kMissingSite text.
    for (const char* keep : {"", "siteLatitude", "siteLongitude"}) {
        INFO(std::string("keeping ") + keep);
        DeviceConfig config = skywatcher_serial_with_site();
        if (std::string_view(keep) != "siteLatitude") config.erase("siteLatitude");
        if (std::string_view(keep) != "siteLongitude") config.erase("siteLongitude");
        const auto api = catalog.normalize(kSkyWatcherKey, config, Source::Api);
        REQUIRE(api.rejection.has_value());
        CHECK(*api.rejection == kSkyWatcherMissingSite);

        // Rule 4, Persisted: no warning, no rejection, the config as it is. The
        // factory (not the schema) logs the WARNING, since only a saved config
        // reaches it with a coordinate missing.
        const auto persisted = catalog.normalize(kSkyWatcherKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        CHECK(persisted.warnings.empty());
        CHECK(persisted.config.has("siteLatitude") == config.has("siteLatitude"));
        CHECK(persisted.config.has("siteLongitude") == config.has("siteLongitude"));
        CHECK(string_at(persisted.config, "connectionType") == "serial");
        CHECK(string_at(persisted.config, "portPath") == "/dev/ttyUSB8");
    }

    // Null island is a real place: the rule is about presence, not value.
    DeviceConfig null_island = skywatcher_serial_with_site();
    null_island.set("siteLatitude", 0.0);
    null_island.set("siteLongitude", 0.0);
    CHECK_FALSE(catalog.normalize(kSkyWatcherKey, null_island, Source::Api).rejection.has_value());

    // The site check comes first among the cross-field rules, as in the arm:
    // a config missing its site AND carrying a bad connection type or an empty
    // endpoint is refused for the site.
    DeviceConfig no_site_bad_type;
    no_site_bad_type.set("connectionType", std::string{"carrier-pigeon"});
    no_site_bad_type.set("portPath", std::string{"/dev/ttyUSB9"});
    const auto bad_type = catalog.normalize(kSkyWatcherKey, no_site_bad_type, Source::Api);
    REQUIRE(bad_type.rejection.has_value());
    CHECK(*bad_type.rejection == kSkyWatcherMissingSite);
    DeviceConfig no_site_no_port;
    no_site_no_port.set("connectionType", std::string{"serial"});
    const auto no_port = catalog.normalize(kSkyWatcherKey, no_site_no_port, Source::Api);
    REQUIRE(no_port.rejection.has_value());
    CHECK(*no_port.rejection == kSkyWatcherMissingSite);
}

TEST_CASE(
    "Builtin catalog - SkyWatcher normalize accepts four connection types and reads any other as serial when saved",
    "[catalog][skywatcher][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    // Rule 5: "", "auto", "serial" and "network" are accepted from both
    // sources and left exactly as given ("" stays "", it is not rewritten to "auto").
    for (const char* type : {"", "auto", "serial", "network"}) {
        INFO(std::string("connectionType \"") + type + "\"");
        DeviceConfig config = skywatcher_serial_with_site();
        config.set("connectionType", std::string{type});
        config.set("host", std::string{"192.168.4.1"});
        config.set("mountIndex", std::int64_t{2});
        const auto api = catalog.normalize(kSkyWatcherKey, config, Source::Api);
        CHECK_FALSE(api.rejection.has_value());
        CHECK(string_at(api.config, "connectionType") == type);
        const auto persisted = catalog.normalize(kSkyWatcherKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        CHECK(persisted.warnings.empty());
        CHECK(string_at(persisted.config, "connectionType") == type);
    }

    // Any other value: the API gets the arm's exact text; a saved one is read
    // as "serial" (never "auto", #380: a saved unknown type must not
    // auto-probe) with a warning that keeps the arm's two fragments, which
    // the router wraps as "... config normalized: <warning>. ...".
    for (const char* type : {"carrier-pigeon", "Network", "SERIAL"}) {
        INFO(std::string("connectionType \"") + type + "\"");
        DeviceConfig config = skywatcher_serial_with_site();
        config.set("connectionType", std::string{type});
        const auto api = catalog.normalize(kSkyWatcherKey, config, Source::Api);
        REQUIRE(api.rejection.has_value());
        CHECK(*api.rejection == "Invalid connection type. Use 'auto', 'serial', or 'network'");
        // A rejection returns the config as given.
        CHECK(string_at(api.config, "connectionType") == type);

        const auto persisted = catalog.normalize(kSkyWatcherKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        CHECK(string_at(persisted.config, "connectionType") == "serial");
        REQUIRE(persisted.warnings.size() == 1);
        CHECK(persisted.warnings[0].find(std::string("has connectionType \"") + type + "\"") != std::string::npos);
        CHECK(persisted.warnings[0].find("treating it as \"serial\"") != std::string::npos);
        CHECK(persisted.warnings[0].find("\"auto\"") == std::string::npos);
    }

    // The substitution feeds rule 6: a saved unknown type with no port path is
    // read as serial AND warned about for the missing port, as the arm did.
    DeviceConfig no_port;
    no_port.set("connectionType", std::string{"carrier-pigeon"});
    no_port.set("siteLatitude", 39.7392);
    no_port.set("siteLongitude", -104.9903);
    const auto persisted = catalog.normalize(kSkyWatcherKey, no_port, Source::Persisted);
    CHECK_FALSE(persisted.rejection.has_value());
    CHECK(any_contains(persisted.warnings, "has connectionType \"carrier-pigeon\""));
    CHECK(any_contains(persisted.warnings, "Serial port path is required"));
}

TEST_CASE("Builtin catalog - SkyWatcher normalize refuses an empty endpoint and the catalog warns for a saved one",
          "[catalog][skywatcher][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    struct Case {
        const char* type;
        const char* endpoint_key;
        bool set_empty;  // true: key present and ""; false: key absent
        const char* text;
    };
    const Case cases[]{
        {"serial", "portPath", true, "Serial port path is required"},
        {"serial", "portPath", false, "Serial port path is required"},
        {"network", "host", true, "Host IP address is required"},
        {"network", "host", false, "Host IP address is required"},
    };
    for (const Case& c : cases) {
        INFO(std::string(c.type) + " with " + (c.set_empty ? "empty " : "absent ") + c.endpoint_key);
        DeviceConfig config;
        config.set("connectionType", std::string{c.type});
        if (c.set_empty) config.set(c.endpoint_key, std::string{});
        config.set("siteLatitude", 39.7392);
        config.set("siteLongitude", -104.9903);

        // Rule 6, Api: the arm's exact text.
        const auto api = catalog.normalize(kSkyWatcherKey, config, Source::Api);
        REQUIRE(api.rejection.has_value());
        CHECK(*api.rejection == c.text);

        // Rule 6, Persisted: the schema's rejection becomes a catalog warning
        // (device_catalog.cpp turns a persisted cross-field rejection into a
        // warning and keeps the per-field result), so the device registers.
        const auto persisted = catalog.normalize(kSkyWatcherKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        REQUIRE(persisted.warnings.size() == 1);
        CHECK(persisted.warnings[0] == c.text);
        CHECK(string_at(persisted.config, "connectionType") == c.type);
        CHECK(persisted.config.has(c.endpoint_key) == c.set_empty);
    }

    // The other endpoint is not consulted: a serial config with no host and a
    // network config with no port path are both fine.
    DeviceConfig serial = skywatcher_serial_with_site();
    CHECK_FALSE(catalog.normalize(kSkyWatcherKey, serial, Source::Api).rejection.has_value());
    DeviceConfig network;
    network.set("connectionType", std::string{"network"});
    network.set("host", std::string{"192.168.4.1"});
    network.set("siteLatitude", -33.87);
    network.set("siteLongitude", 151.21);
    CHECK_FALSE(catalog.normalize(kSkyWatcherKey, network, Source::Api).rejection.has_value());
    // Auto never needs an endpoint.
    DeviceConfig automatic;
    automatic.set("connectionType", std::string{"auto"});
    automatic.set("siteLatitude", -33.87);
    automatic.set("siteLongitude", 151.21);
    CHECK_FALSE(catalog.normalize(kSkyWatcherKey, automatic, Source::Api).rejection.has_value());
}

TEST_CASE(
    "Builtin catalog - SkyWatcher normalize refuses an unknown decAxisSense from the API and drops a saved one (#582)",
    "[catalog][skywatcher][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    for (const char* good : {"auto", "normal", "reversed"}) {
        INFO(good);
        DeviceConfig config = skywatcher_serial_with_site();
        config.set("decAxisSense", std::string{good});
        const auto api = catalog.normalize(kSkyWatcherKey, config, Source::Api);
        CHECK_FALSE(api.rejection.has_value());
        CHECK(string_at(api.config, "decAxisSense") == good);
    }

    DeviceConfig bogus = skywatcher_serial_with_site();
    bogus.set("decAxisSense", std::string{"bogus"});
    const auto api = catalog.normalize(kSkyWatcherKey, bogus, Source::Api);
    REQUIRE(api.rejection.has_value());
    CHECK(*api.rejection == "Invalid dec axis sense. Use 'auto', 'normal', or 'reversed'");

    const auto persisted = catalog.normalize(kSkyWatcherKey, bogus, Source::Persisted);
    CHECK_FALSE(persisted.rejection.has_value());
    REQUIRE(persisted.warnings.size() == 1);
    CHECK(persisted.warnings[0].find("decAxisSense \"bogus\"") != std::string::npos);
    CHECK_FALSE(persisted.config.has("decAxisSense"));
}

TEST_CASE("Builtin catalog - SkyWatcher site range is per-field and precedes the missing-site rule (#398, #508 item 5)",
          "[catalog][skywatcher][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    // Rule 7, Api: the per-field range message (the catalog's wording, which
    // replaces read_site_coordinates()' "200.000000 ... degrees" text) wins over
    // the cross-field missing-site rule, because per-field rules run first.
    struct Bad {
        const char* key;
        double value;
        const char* message;
    };
    const Bad bad[]{
        {"siteLatitude", 200.0, "siteLatitude is out of range (min -90) (max 90)"},
        {"siteLatitude", -90.5, "siteLatitude is out of range (min -90) (max 90)"},
        {"siteLongitude", 999.0, "siteLongitude is out of range (min -180) (max 180)"},
        {"siteLongitude", -180.5, "siteLongitude is out of range (min -180) (max 180)"},
        {"siteLatitude", std::numeric_limits<double>::quiet_NaN(), "siteLatitude is out of range (min -90) (max 90)"},
    };
    for (const Bad& b : bad) {
        INFO(std::string(b.key) + " = " + std::to_string(b.value));
        DeviceConfig config = skywatcher_serial_with_site();
        config.set(b.key, b.value);
        const auto api = catalog.normalize(kSkyWatcherKey, config, Source::Api);
        REQUIRE(api.rejection.has_value());
        CHECK(*api.rejection == b.message);

        // Persisted: the value is dropped to unset with that one warning, and
        // rule 4 adds NO missing-site warning of its own -- the factory logs
        // it when it builds the driver (see the factory case below).
        const auto persisted = catalog.normalize(kSkyWatcherKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        REQUIRE(persisted.warnings.size() == 1);
        CHECK(persisted.warnings[0] == b.message);
        CHECK_FALSE(persisted.config.has(b.key));
        CHECK(persisted.config.has(std::string_view(b.key) == "siteLatitude" ? "siteLongitude" : "siteLatitude"));
        CHECK_FALSE(any_contains(persisted.warnings, "Site latitude and longitude are required"));
    }

    // The limits are inclusive: the poles and the antimeridian are real places.
    for (const auto& [key, value] : std::vector<std::pair<const char*, double>>{
             {"siteLatitude", 90.0}, {"siteLatitude", -90.0}, {"siteLongitude", 180.0}, {"siteLongitude", -180.0}}) {
        INFO(std::string(key) + " = " + std::to_string(value));
        DeviceConfig config = skywatcher_serial_with_site();
        config.set(key, value);
        CHECK_FALSE(catalog.normalize(kSkyWatcherKey, config, Source::Api).rejection.has_value());
    }

    // An integer literal in a double field is a valid double (device_catalog.cpp
    // widens it before the range check): {"siteLatitude": 40} is not a type error.
    DeviceConfig whole = skywatcher_serial_with_site();
    whole.set("siteLatitude", std::int64_t{40});
    const auto widened = catalog.normalize(kSkyWatcherKey, whole, Source::Api);
    CHECK_FALSE(widened.rejection.has_value());
    const ConfigValue* lat = widened.config.find_value("siteLatitude");
    REQUIRE(lat != nullptr);
    REQUIRE(std::holds_alternative<double>(*lat));
    CHECK(std::get<double>(*lat) == 40.0);
}

TEST_CASE("Builtin catalog - SkyWatcher sanitize keeps every declared field whatever the connection type (ADR 0004)",
          "[catalog][skywatcher][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    // Rule 8: "Sanitize keeps every declared non-secret field; only the UI
    // honours applies_when". The arm kept portPath/baudRate only for "serial"
    // and host/udpPort only for "network"; the catalog keeps all four either
    // way. tcpPort is not a Sky-Watcher field and still drops.
    for (const char* type : {"serial", "network", "auto", ""}) {
        INFO(std::string("connectionType \"") + type + "\"");
        DeviceConfig config;
        config.set("connectionType", std::string{type});
        config.set("mountIndex", std::int64_t{1});
        config.set("portPath", std::string{"/dev/ttyUSB6"});
        config.set("baudRate", std::int64_t{9600});
        config.set("host", std::string{"192.168.4.1"});
        config.set("udpPort", std::int64_t{11880});
        config.set("responseTimeoutMs", std::int64_t{4000});
        config.set("siteLatitude", 39.7392);
        config.set("siteLongitude", -104.9903);
        config.set("siteElevation", 1609.0);
        config.set("apertureDiameter", 0.2);
        config.set("focalLength", 1.0);
        config.set("tcpPort", std::int64_t{1});
        config.set("junk", true);
        const DeviceConfig sanitized = catalog.sanitize(kSkyWatcherKey, config);
        for (const char* key :
             {"connectionType", "mountIndex", "portPath", "baudRate", "host", "udpPort", "responseTimeoutMs",
              "siteLatitude", "siteLongitude", "siteElevation", "apertureDiameter", "focalLength"}) {
            INFO(key);
            CHECK(sanitized.has(key));
        }
        CHECK_FALSE(sanitized.has("tcpPort"));
        CHECK_FALSE(sanitized.has("junk"));
        CHECK(sanitized.entries().size() == 12);
    }
}

#ifdef ALPACACORE_ENABLE_SKYWATCHER

namespace {

// Every WARN the block logs, by message (the component is not part of the contract).
struct WarnCapture {
    std::vector<std::string> messages;
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    WarnCapture() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn) messages.emplace_back(message);
            });
    }
    ~WarnCapture() { alpacacore::logging::set_log_sink(previous); }
    WarnCapture(const WarnCapture&) = delete;
    WarnCapture& operator=(const WarnCapture&) = delete;
};

}  // namespace

TEST_CASE("Builtin catalog - the SkyWatcher factory passes the values through and constructs without a scan",
          "[catalog][skywatcher][unit]") {
    const DeviceCatalog catalog = builtin_catalog();

    // Rule 9, serial: create_skywatcher_telescope with the endpoint and site;
    // apertureDiameter and focalLength applied because they are > 0.
    {
        const std::string path = "/dev/ttyUSB-skywatcher-none";
        DeviceConfig config;
        config.set("connectionType", std::string{"serial"});
        config.set("portPath", path);
        config.set("baudRate", std::int64_t{115200});
        config.set("responseTimeoutMs", std::int64_t{250});
        config.set("siteLatitude", -33.87);
        config.set("siteLongitude", 151.21);
        config.set("siteElevation", 42.0);
        config.set("apertureDiameter", 0.2);
        config.set("focalLength", 1.0);
        WarnCapture warns;
        std::unique_ptr<AlpacaDriver> driver;
        REQUIRE_NOTHROW(driver = catalog.create(kSkyWatcherKey, config, 7));
        REQUIRE(driver != nullptr);
        CHECK(driver->get_device_type() == DeviceType::Telescope);
        CHECK(driver->get_device_number() == 7);
        CHECK_FALSE(driver->get_connected());
        auto* telescope = dynamic_cast<TelescopeDriver*>(driver.get());
        REQUIRE(telescope != nullptr);
        // The site getters return the stored value under the mutex with no
        // connection check (the #398 test_routing case relies on the same).
        CHECK(telescope->get_site_latitude() == -33.87);
        CHECK(telescope->get_site_longitude() == 151.21);
        CHECK(telescope->get_site_elevation() == 42.0);
        CHECK(telescope->get_aperture_diameter() == 0.2);
        CHECK(telescope->get_focal_length() == 1.0);
        // A complete saved config logs nothing (rule 4's WARNING is for a missing site only).
        CHECK(warns.messages.empty());
        // The node does not exist: the connect is refused, and by the serial
        // path, not by the auto-detect scan.
        try {
            driver->set_connected(true);
            FAIL("set_connected(true) must throw with no motor controller on " + path);
        } catch (const AlpacaException& e) {
            INFO(e.what());
            CHECK_FALSE(driver->get_connected());
            CHECK(std::string(e.what()).find("auto-detect") == std::string::npos);
            CHECK(std::string(e.what()).find("Site latitude and longitude must be set") == std::string::npos);
        }
    }

    // Rule 9, the arm's "applied only when > 0": zero and absent leave the
    // driver's own defaults (the setters would accept 0.0, so this is the
    // factory's choice, not the driver's).
    {
        DeviceConfig defaults = skywatcher_serial_with_site();
        defaults.set("apertureDiameter", 0.0);
        std::unique_ptr<AlpacaDriver> zero = catalog.create(kSkyWatcherKey, defaults, 8);
        DeviceConfig absent = skywatcher_serial_with_site();
        std::unique_ptr<AlpacaDriver> untouched = catalog.create(kSkyWatcherKey, absent, 9);
        auto* a = dynamic_cast<TelescopeDriver*>(zero.get());
        auto* b = dynamic_cast<TelescopeDriver*>(untouched.get());
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(a->get_aperture_diameter() == b->get_aperture_diameter());
        CHECK(a->get_focal_length() == b->get_focal_length());
    }

    // Rule 9, network: create_skywatcher_telescope with the host; constructs
    // without touching the network.
    {
        DeviceConfig config;
        config.set("connectionType", std::string{"network"});
        config.set("host", std::string{"192.0.2.1"});  // TEST-NET-1: never routable
        config.set("udpPort", std::int64_t{11880});
        config.set("siteLatitude", 39.7392);
        config.set("siteLongitude", -104.9903);
        std::unique_ptr<AlpacaDriver> driver;
        REQUIRE_NOTHROW(driver = catalog.create(kSkyWatcherKey, config, 10));
        REQUIRE(driver != nullptr);
        CHECK_FALSE(driver->get_connected());
    }

    // Rule 9, auto ("" and "auto"): create_skywatcher_telescope_auto. The
    // #659/#660 contract: construction must not scan. A scan here with no
    // hardware would throw "nothing answered the auto-detect probe"; the
    // resolver runs only inside Connected=true, which this case never issues
    // (a real scan DTR-resets every CP210x/CH340 on the box).
    for (const char* type : {"", "auto"}) {
        INFO(std::string("connectionType \"") + type + "\"");
        DeviceConfig config;
        config.set("connectionType", std::string{type});
        config.set("mountIndex", std::int64_t{3});
        config.set("siteLatitude", 39.7392);
        config.set("siteLongitude", -104.9903);
        config.set("siteElevation", 1609.0);
        std::unique_ptr<AlpacaDriver> driver;
        REQUIRE_NOTHROW(driver = catalog.create(kSkyWatcherKey, config, 11));
        REQUIRE(driver != nullptr);
        CHECK(driver->get_device_type() == DeviceType::Telescope);
        CHECK_FALSE(driver->get_connected());
        auto* telescope = dynamic_cast<TelescopeDriver*>(driver.get());
        REQUIRE(telescope != nullptr);
        CHECK(telescope->get_site_latitude() == 39.7392);
        CHECK(telescope->get_site_longitude() == -104.9903);
        CHECK(telescope->get_site_elevation() == 1609.0);
    }
}

TEST_CASE("Builtin catalog - the SkyWatcher factory warns about a saved config with no site and still builds it (#274)",
          "[catalog][skywatcher][unit]") {
    const DeviceCatalog catalog = builtin_catalog();

    // Rule 4: only a saved config reaches the factory with a coordinate
    // missing (the API is refused in normalize). The factory logs the arm's
    // exact WARNING, names the missing half, and builds the driver, which
    // refuses the connect itself.
    struct Case {
        bool latitude;
        bool longitude;
        const char* missing;
    };
    const Case cases[]{
        {false, false, "site coordinates"},
        {true, false, "site longitude"},
        {false, true, "site latitude"},
    };
    int device_number = 20;
    for (const Case& c : cases) {
        INFO(c.missing);
        DeviceConfig config;
        config.set("connectionType", std::string{"serial"});
        config.set("portPath", std::string{"/dev/ttyUSB-skywatcher-none"});
        if (c.latitude) config.set("siteLatitude", 39.7392);
        if (c.longitude) config.set("siteLongitude", -104.9903);
        const int number = device_number++;
        const std::string expected = "Persisted Sky-Watcher telescope " + std::to_string(number) + " has no " +
                                     c.missing + " and will refuse to connect. " + kSkyWatcherMissingSite;

        WarnCapture warns;
        std::unique_ptr<AlpacaDriver> driver;
        REQUIRE_NOTHROW(driver = catalog.create(kSkyWatcherKey, config, number));
        REQUIRE(driver != nullptr);
        REQUIRE(warns.messages.size() == 1);
        CHECK(warns.messages[0] == expected);

        CHECK_FALSE(driver->get_connected());
        try {
            driver->set_connected(true);
            FAIL("set_connected(true) must be refused without a site (#274)");
        } catch (const AlpacaException& e) {
            INFO(e.what());
            CHECK_FALSE(driver->get_connected());
            CHECK(std::string(e.what()).find("Site latitude and longitude must be set before connecting") !=
                  std::string::npos);
        }
    }
}

#endif  // ALPACACORE_ENABLE_SKYWATCHER

// ---------------------------------------------------------------------------
// Bisque / Paramount (TheSkyX) telescope
// ---------------------------------------------------------------------------

namespace {

const DeviceKey kBisqueKey{"bisque", DeviceType::Telescope};
const std::string kBisqueHostMissing = "Host is required for Bisque/TheSkyX connection";

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the Bisque telescope in every build",
          "[catalog][bisque][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kBisqueKey);
    REQUIRE(v != nullptr);
    // The first word is exactly "Bisque" so vendor_label() keeps the arm's texts.
    CHECK(v->display_name == "Bisque Paramount (TheSkyX)");
    CHECK(v->build_option == "ALPACACORE_ENABLE_BISQUE");

    // An absent host reads as the arm's default "localhost", so it is not refused.
    CHECK_FALSE(catalog.normalize(kBisqueKey, DeviceConfig{}, Source::Api).rejection.has_value());
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the Bisque telescope available only when built",
          "[catalog][bisque][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kBisqueKey);
    REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_BISQUE
    CHECK(v->available);
#else
    CHECK_FALSE(v->available);
    try {
        (void)catalog.create(kBisqueKey, DeviceConfig{}, 0);
        FAIL("create() must throw when Bisque is not built");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_BISQUE") != std::string::npos);
    }
#endif
}

TEST_CASE("Builtin catalog - Bisque normalize refuses an empty host from the API and warns for a saved config",
          "[catalog][bisque][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    DeviceConfig config;
    config.set("host", std::string{""});

    const auto api = catalog.normalize(kBisqueKey, config, Source::Api);
    REQUIRE(api.rejection.has_value());
    CHECK(*api.rejection == kBisqueHostMissing);

    // A saved config is registered anyway so it stays editable in the web UI.
    const auto persisted = catalog.normalize(kBisqueKey, config, Source::Persisted);
    CHECK_FALSE(persisted.rejection.has_value());
    CHECK(any_contains(persisted.warnings, kBisqueHostMissing));

    config.set("host", std::string{"skyx.local"});
    const auto ok = catalog.normalize(kBisqueKey, config, Source::Api);
    CHECK_FALSE(ok.rejection.has_value());
    CHECK(string_at(ok.config, "host") == "skyx.local");
}

// ---------------------------------------------------------------------------
// OnStep telescope. Serial only (no "network"); the auto-detect factory
// constructs without a scan (#659), so no port is opened here.
// ---------------------------------------------------------------------------

namespace {

const DeviceKey kOnStepKey{"onstep", DeviceType::Telescope};

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the OnStep telescope in every build",
          "[catalog][onstep][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kOnStepKey);
    REQUIRE(v != nullptr);
    // The first word is exactly "OnStep" so vendor_label() keeps the arm's texts.
    CHECK(v->display_name == "OnStep");
    CHECK(v->build_option == "ALPACACORE_ENABLE_ONSTEP");
    CHECK_FALSE(v->available);  // schemas only: no factory has been registered yet

    const std::vector<std::string_view> expected_keys{
        "connectionType", "mountIndex",    "portPath",          "baudRate",         "responseTimeoutMs", "siteLatitude",
        "siteLongitude",  "siteElevation", "syncTimeOnConnect", "apertureDiameter", "focalLength"};
    REQUIRE(v->fields.size() == expected_keys.size());
    for (std::size_t i = 0; i < expected_keys.size(); ++i) {
        CHECK(std::string_view(v->fields[i].key) == expected_keys[i]);
    }
    const FieldRef* type = find_field(v->fields, "connectionType");
    REQUIRE(type != nullptr);
    CHECK(type->allowed_values.empty());  // normalize owns the rule (#380)
    CHECK(same_scalar(type->default_value, std::string{"auto"}));
    const FieldRef* timeout = find_field(v->fields, "responseTimeoutMs");
    REQUIRE(timeout != nullptr);
    CHECK(same_scalar(timeout->default_value, std::int64_t{5000}));
    const FieldRef* port = find_field(v->fields, "portPath");
    REQUIRE(port != nullptr);
    REQUIRE(port->applies_when.has_value());
    CHECK(std::string_view(port->applies_when->value) == "serial");
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the OnStep telescope available only when built",
          "[catalog][onstep][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kOnStepKey);
    REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_ONSTEP
    CHECK(v->available);
    // Auto-detect is lazy: constructing it scans nothing and succeeds with no mount.
    DeviceConfig config;
    config.set("connectionType", std::string{"auto"});
    auto driver = catalog.create(kOnStepKey, config, 4);
    REQUIRE(driver != nullptr);
    CHECK(driver->get_device_type() == DeviceType::Telescope);
    CHECK(driver->get_device_number() == 4);
    CHECK_FALSE(driver->get_connected());

    DeviceConfig serial;
    serial.set("connectionType", std::string{"serial"});
    serial.set("portPath", std::string{"/dev/ttyUSB-no-such-onstep"});
    auto serial_driver = catalog.create(kOnStepKey, serial, 5);
    REQUIRE(serial_driver != nullptr);
    CHECK(serial_driver->get_device_number() == 5);
#else
    CHECK_FALSE(v->available);
    try {
        (void)catalog.create(kOnStepKey, DeviceConfig{}, 0);
        FAIL("create() must throw when OnStep is not built");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_ONSTEP") != std::string::npos);
    }
#endif
}

TEST_CASE("Builtin catalog - OnStep normalize accepts '', auto and serial, and refuses network from the API",
          "[catalog][onstep][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    for (const char* type : {"", "auto", "serial"}) {
        INFO(std::string("connectionType \"") + type + "\"");
        DeviceConfig config;
        config.set("connectionType", std::string{type});
        config.set("portPath", std::string{"/dev/ttyUSB3"});
        const auto api = catalog.normalize(kOnStepKey, config, Source::Api);
        CHECK_FALSE(api.rejection.has_value());
        CHECK(string_at(api.config, "connectionType") == type);
        const auto persisted = catalog.normalize(kOnStepKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        CHECK(persisted.warnings.empty());
    }

    // No site is required: the arm never asked for one.
    DeviceConfig no_site;
    no_site.set("connectionType", std::string{"auto"});
    CHECK_FALSE(catalog.normalize(kOnStepKey, no_site, Source::Api).rejection.has_value());

    for (const char* type : {"network", "carrier-pigeon", "SERIAL"}) {
        INFO(std::string("connectionType \"") + type + "\"");
        DeviceConfig config;
        config.set("connectionType", std::string{type});
        config.set("portPath", std::string{"/dev/ttyUSB3"});
        const auto api = catalog.normalize(kOnStepKey, config, Source::Api);
        REQUIRE(api.rejection.has_value());
        CHECK(*api.rejection == "Invalid connection type. Use 'auto' or 'serial'");

        // #380: a saved one is read as serial, never auto.
        const auto persisted = catalog.normalize(kOnStepKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        CHECK(string_at(persisted.config, "connectionType") == "serial");
        REQUIRE(persisted.warnings.size() == 1);
        CHECK(persisted.warnings[0].find(std::string("has connectionType \"") + type + "\"") != std::string::npos);
        CHECK(persisted.warnings[0].find("treating it as \"serial\"") != std::string::npos);
    }
}

TEST_CASE("Builtin catalog - OnStep normalize requires a serial port path from the API only",
          "[catalog][onstep][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    for (bool set_empty : {true, false}) {
        INFO(std::string(set_empty ? "empty" : "absent") + " portPath");
        DeviceConfig config;
        config.set("connectionType", std::string{"serial"});
        if (set_empty) config.set("portPath", std::string{});
        const auto api = catalog.normalize(kOnStepKey, config, Source::Api);
        REQUIRE(api.rejection.has_value());
        CHECK(*api.rejection == "Serial port path is required");
        const auto persisted = catalog.normalize(kOnStepKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        REQUIRE(persisted.warnings.size() == 1);
        CHECK(persisted.warnings[0] == "Serial port path is required");
    }

    // A saved unknown type is read as serial, so it is warned about the port too.
    DeviceConfig unknown;
    unknown.set("connectionType", std::string{"network"});
    const auto persisted = catalog.normalize(kOnStepKey, unknown, Source::Persisted);
    CHECK(any_contains(persisted.warnings, "has connectionType \"network\""));
    CHECK(any_contains(persisted.warnings, "Serial port path is required"));

    // Auto never needs a port.
    DeviceConfig automatic;
    automatic.set("connectionType", std::string{"auto"});
    CHECK_FALSE(catalog.normalize(kOnStepKey, automatic, Source::Api).rejection.has_value());
}

TEST_CASE("Builtin catalog - OnStep site range is per-field", "[catalog][onstep][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    DeviceConfig config;
    config.set("connectionType", std::string{"auto"});
    config.set("siteLatitude", 91.0);
    const auto api = catalog.normalize(kOnStepKey, config, Source::Api);
    REQUIRE(api.rejection.has_value());
    CHECK(*api.rejection == "siteLatitude is out of range (min -90) (max 90)");
    config.set("siteLatitude", 90.0);
    config.set("siteLongitude", -180.0);
    CHECK_FALSE(catalog.normalize(kOnStepKey, config, Source::Api).rejection.has_value());
}

// ---------------------------------------------------------------------------
// Celestron telescope. Serial, network and auto; the auto-detect factory
// constructs without a scan (#659), so no port is opened here.
// ---------------------------------------------------------------------------

namespace {

const DeviceKey kCelestronKey{"celestron", DeviceType::Telescope};

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the Celestron telescope in every build",
          "[catalog][celestron][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kCelestronKey);
    REQUIRE(v != nullptr);
    CHECK(v->display_name == "Celestron");
    CHECK(v->build_option == "ALPACACORE_ENABLE_CELESTRON");
    CHECK_FALSE(v->available);  // schemas only: no factory has been registered yet

    const std::vector<std::string_view> expected_keys{
        "connectionType", "mountIndex",        "portPath",         "baudRate",     "host",
        "tcpPort",        "responseTimeoutMs", "alignmentMode",    "siteLatitude", "siteLongitude",
        "siteElevation",  "syncTimeOnConnect", "apertureDiameter", "focalLength"};
    REQUIRE(v->fields.size() == expected_keys.size());
    for (std::size_t i = 0; i < expected_keys.size(); ++i) {
        CHECK(std::string_view(v->fields[i].key) == expected_keys[i]);
    }
    const FieldRef* type = find_field(v->fields, "connectionType");
    REQUIRE(type != nullptr);
    CHECK(type->allowed_values.empty());  // normalize owns the rule (#380)
    const FieldRef* tcp = find_field(v->fields, "tcpPort");
    REQUIRE(tcp != nullptr);
    CHECK(same_scalar(tcp->default_value, std::int64_t{2000}));
    REQUIRE(tcp->applies_when.has_value());
    CHECK(std::string_view(tcp->applies_when->value) == "network");
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the Celestron telescope available only when built",
          "[catalog][celestron][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kCelestronKey);
    REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_CELESTRON
    CHECK(v->available);
    // Auto-detect is lazy: constructing it scans nothing and succeeds with no mount.
    DeviceConfig config;
    config.set("connectionType", std::string{"auto"});
    config.set("alignmentMode", std::string{"equatorial"});
    auto driver = catalog.create(kCelestronKey, config, 4);
    REQUIRE(driver != nullptr);
    CHECK(driver->get_device_type() == DeviceType::Telescope);
    CHECK(driver->get_device_number() == 4);
    CHECK_FALSE(driver->get_connected());

    DeviceConfig serial;
    serial.set("connectionType", std::string{"serial"});
    serial.set("portPath", std::string{"/dev/ttyUSB-no-such-celestron"});
    auto serial_driver = catalog.create(kCelestronKey, serial, 5);
    REQUIRE(serial_driver != nullptr);
    CHECK(serial_driver->get_device_number() == 5);

    DeviceConfig network;
    network.set("connectionType", std::string{"network"});
    network.set("host", std::string{"192.0.2.1"});
    auto network_driver = catalog.create(kCelestronKey, network, 6);
    REQUIRE(network_driver != nullptr);
    CHECK(network_driver->get_device_number() == 6);
#else
    CHECK_FALSE(v->available);
    try {
        (void)catalog.create(kCelestronKey, DeviceConfig{}, 0);
        FAIL("create() must throw when Celestron is not built");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_CELESTRON") != std::string::npos);
    }
#endif
}

TEST_CASE("Builtin catalog - Celestron normalize accepts '', auto, serial and network, refuses others from the API",
          "[catalog][celestron][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    for (const char* type : {"", "auto", "serial", "network"}) {
        INFO(std::string("connectionType \"") + type + "\"");
        DeviceConfig config;
        config.set("connectionType", std::string{type});
        config.set("portPath", std::string{"/dev/ttyUSB3"});
        config.set("host", std::string{"192.0.2.1"});
        const auto api = catalog.normalize(kCelestronKey, config, Source::Api);
        CHECK_FALSE(api.rejection.has_value());
        const auto persisted = catalog.normalize(kCelestronKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        CHECK(persisted.warnings.empty());
    }

    for (const char* type : {"carrier-pigeon", "SERIAL"}) {
        INFO(std::string("connectionType \"") + type + "\"");
        DeviceConfig config;
        config.set("connectionType", std::string{type});
        config.set("portPath", std::string{"/dev/ttyUSB3"});
        const auto api = catalog.normalize(kCelestronKey, config, Source::Api);
        REQUIRE(api.rejection.has_value());
        CHECK(*api.rejection == "Invalid connection type. Use 'auto', 'serial', or 'network'");

        // #380: a saved one is read as serial, never auto.
        const auto persisted = catalog.normalize(kCelestronKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        CHECK(string_at(persisted.config, "connectionType") == "serial");
        REQUIRE(persisted.warnings.size() == 1);
        CHECK(persisted.warnings[0].find(std::string("has connectionType \"") + type + "\"") != std::string::npos);
    }
}

TEST_CASE("Builtin catalog - Celestron normalize requires the endpoint from the API only",
          "[catalog][celestron][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    struct Case {
        const char* type;
        const char* message;
    };
    for (const Case& c :
         {Case{"serial", "Serial port path is required"}, Case{"network", "Host IP address is required"}}) {
        INFO(c.type);
        DeviceConfig config;
        config.set("connectionType", std::string{c.type});
        const auto api = catalog.normalize(kCelestronKey, config, Source::Api);
        REQUIRE(api.rejection.has_value());
        CHECK(*api.rejection == c.message);
        const auto persisted = catalog.normalize(kCelestronKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        REQUIRE(persisted.warnings.size() == 1);
        CHECK(persisted.warnings[0] == c.message);
    }

    // Auto never needs an endpoint, and no site is required.
    DeviceConfig automatic;
    automatic.set("connectionType", std::string{"auto"});
    CHECK_FALSE(catalog.normalize(kCelestronKey, automatic, Source::Api).rejection.has_value());
}

TEST_CASE("Builtin catalog - Celestron alignmentMode keeps a known value and drops an unknown one (#860)",
          "[catalog][celestron][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    for (const char* mode : {"auto", "altaz", "equatorial"}) {
        INFO(mode);
        DeviceConfig config;
        config.set("connectionType", std::string{"auto"});
        config.set("alignmentMode", std::string{mode});
        for (Source source : {Source::Api, Source::Persisted}) {
            const auto r = catalog.normalize(kCelestronKey, config, source);
            CHECK_FALSE(r.rejection.has_value());
            CHECK(string_at(r.config, "alignmentMode") == mode);
        }
    }
    DeviceConfig unknown;
    unknown.set("connectionType", std::string{"auto"});
    unknown.set("alignmentMode", std::string{"wedge"});
    for (Source source : {Source::Api, Source::Persisted}) {
        const auto r = catalog.normalize(kCelestronKey, unknown, source);
        CHECK_FALSE(r.rejection.has_value());
        CHECK(r.config.find_value("alignmentMode") == nullptr);
    }
}

TEST_CASE("Builtin catalog - Celestron site range is per-field", "[catalog][celestron][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    DeviceConfig config;
    config.set("connectionType", std::string{"auto"});
    config.set("siteLongitude", 181.0);
    const auto api = catalog.normalize(kCelestronKey, config, Source::Api);
    REQUIRE(api.rejection.has_value());
    CHECK(*api.rejection == "siteLongitude is out of range (min -180) (max 180)");
    config.set("siteLongitude", 180.0);
    CHECK_FALSE(catalog.normalize(kCelestronKey, config, Source::Api).rejection.has_value());
}

// ---------------------------------------------------------------------------
// SynScan telescope. Serial, network and auto; the auto-detect factory
// constructs without a scan (#659), so no port is opened here.
// ---------------------------------------------------------------------------

namespace {

const DeviceKey kSynScanKey{"synscan", DeviceType::Telescope};

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the SynScan telescope in every build",
          "[catalog][synscan][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kSynScanKey);
    REQUIRE(v != nullptr);
    CHECK(v->display_name == "SynScan");
    CHECK(v->build_option == "ALPACACORE_ENABLE_SYNSCAN");
    CHECK_FALSE(v->available);  // schemas only: no factory has been registered yet

    const std::vector<std::string_view> expected_keys{
        "connectionType", "mountIndex",    "synscanVersion",    "portPath",         "baudRate",
        "host",           "tcpPort",       "responseTimeoutMs", "alignmentMode",    "siteLatitude",
        "siteLongitude",  "siteElevation", "syncTimeOnConnect", "apertureDiameter", "focalLength"};
    REQUIRE(v->fields.size() == expected_keys.size());
    for (std::size_t i = 0; i < expected_keys.size(); ++i) {
        CHECK(std::string_view(v->fields[i].key) == expected_keys[i]);
    }
    const FieldRef* type = find_field(v->fields, "connectionType");
    REQUIRE(type != nullptr);
    CHECK(type->allowed_values.empty());  // normalize owns the rule (#380)
    const FieldRef* tcp = find_field(v->fields, "tcpPort");
    REQUIRE(tcp != nullptr);
    CHECK(same_scalar(tcp->default_value, std::int64_t{11880}));
    REQUIRE(tcp->applies_when.has_value());
    CHECK(std::string_view(tcp->applies_when->value) == "network");
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the SynScan telescope available only when built",
          "[catalog][synscan][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kSynScanKey);
    REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_SYNSCAN
    CHECK(v->available);
    // Auto-detect is lazy: constructing it scans nothing and succeeds with no mount.
    DeviceConfig config;
    config.set("connectionType", std::string{"auto"});
    config.set("alignmentMode", std::string{"equatorial"});
    auto driver = catalog.create(kSynScanKey, config, 4);
    REQUIRE(driver != nullptr);
    CHECK(driver->get_device_type() == DeviceType::Telescope);
    CHECK(driver->get_device_number() == 4);
    CHECK_FALSE(driver->get_connected());

    DeviceConfig serial;
    serial.set("connectionType", std::string{"serial"});
    serial.set("portPath", std::string{"/dev/ttyUSB-no-such-synscan"});
    auto serial_driver = catalog.create(kSynScanKey, serial, 5);
    REQUIRE(serial_driver != nullptr);
    CHECK(serial_driver->get_device_number() == 5);

    DeviceConfig network;
    network.set("connectionType", std::string{"network"});
    network.set("host", std::string{"192.0.2.1"});
    auto network_driver = catalog.create(kSynScanKey, network, 6);
    REQUIRE(network_driver != nullptr);
    CHECK(network_driver->get_device_number() == 6);
#else
    CHECK_FALSE(v->available);
    try {
        (void)catalog.create(kSynScanKey, DeviceConfig{}, 0);
        FAIL("create() must throw when SynScan is not built");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_SYNSCAN") != std::string::npos);
    }
#endif
}

TEST_CASE("Builtin catalog - SynScan normalize accepts '', auto, serial and network, refuses others from the API",
          "[catalog][synscan][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    for (const char* type : {"", "auto", "serial", "network"}) {
        INFO(std::string("connectionType \"") + type + "\"");
        DeviceConfig config;
        config.set("connectionType", std::string{type});
        config.set("portPath", std::string{"/dev/ttyUSB3"});
        config.set("host", std::string{"192.0.2.1"});
        const auto api = catalog.normalize(kSynScanKey, config, Source::Api);
        CHECK_FALSE(api.rejection.has_value());
        const auto persisted = catalog.normalize(kSynScanKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        CHECK(persisted.warnings.empty());
    }

    for (const char* type : {"carrier-pigeon", "SERIAL"}) {
        INFO(std::string("connectionType \"") + type + "\"");
        DeviceConfig config;
        config.set("connectionType", std::string{type});
        config.set("portPath", std::string{"/dev/ttyUSB3"});
        const auto api = catalog.normalize(kSynScanKey, config, Source::Api);
        REQUIRE(api.rejection.has_value());
        CHECK(*api.rejection == "Invalid connection type. Use 'auto', 'serial', or 'network'");

        // #380: a saved one is read as serial, never auto.
        const auto persisted = catalog.normalize(kSynScanKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        CHECK(string_at(persisted.config, "connectionType") == "serial");
        REQUIRE(persisted.warnings.size() == 1);
        CHECK(persisted.warnings[0].find(std::string("has connectionType \"") + type + "\"") != std::string::npos);
    }
}

TEST_CASE("Builtin catalog - SynScan normalize requires the endpoint from the API only", "[catalog][synscan][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    struct Case {
        const char* type;
        const char* message;
    };
    for (const Case& c :
         {Case{"serial", "Serial port path is required"}, Case{"network", "Host IP address is required"}}) {
        INFO(c.type);
        DeviceConfig config;
        config.set("connectionType", std::string{c.type});
        const auto api = catalog.normalize(kSynScanKey, config, Source::Api);
        REQUIRE(api.rejection.has_value());
        CHECK(*api.rejection == c.message);
        const auto persisted = catalog.normalize(kSynScanKey, config, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        REQUIRE(persisted.warnings.size() == 1);
        CHECK(persisted.warnings[0] == c.message);
    }

    // Auto never needs an endpoint, and no site is required.
    DeviceConfig automatic;
    automatic.set("connectionType", std::string{"auto"});
    CHECK_FALSE(catalog.normalize(kSynScanKey, automatic, Source::Api).rejection.has_value());
}

TEST_CASE("Builtin catalog - SynScan alignmentMode keeps a known value and drops an unknown one (#860)",
          "[catalog][synscan][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    for (const char* mode : {"auto", "altaz", "equatorial"}) {
        INFO(mode);
        DeviceConfig config;
        config.set("connectionType", std::string{"auto"});
        config.set("alignmentMode", std::string{mode});
        for (Source source : {Source::Api, Source::Persisted}) {
            const auto r = catalog.normalize(kSynScanKey, config, source);
            CHECK_FALSE(r.rejection.has_value());
            CHECK(string_at(r.config, "alignmentMode") == mode);
        }
    }
    DeviceConfig unknown;
    unknown.set("connectionType", std::string{"auto"});
    unknown.set("alignmentMode", std::string{"wedge"});
    for (Source source : {Source::Api, Source::Persisted}) {
        const auto r = catalog.normalize(kSynScanKey, unknown, source);
        CHECK_FALSE(r.rejection.has_value());
        CHECK(r.config.find_value("alignmentMode") == nullptr);
    }
}

TEST_CASE("Builtin catalog - SynScan site range is per-field", "[catalog][synscan][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    DeviceConfig config;
    config.set("connectionType", std::string{"auto"});
    config.set("siteLongitude", 181.0);
    const auto api = catalog.normalize(kSynScanKey, config, Source::Api);
    REQUIRE(api.rejection.has_value());
    CHECK(*api.rejection == "siteLongitude is out of range (min -180) (max 180)");
    config.set("siteLongitude", 180.0);
    CHECK_FALSE(catalog.normalize(kSynScanKey, config, Source::Api).rejection.has_value());
}

TEST_CASE("Builtin catalog - SynScan synscanVersion defaults to auto and the factory builds for every spelling",
          "[catalog][synscan][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();
    const DescriptorView* v = find_view(views, kSynScanKey);
    REQUIRE(v != nullptr);
    const FieldRef* version = find_field(v->fields, "synscanVersion");
    REQUIRE(version != nullptr);
    CHECK(same_scalar(version->default_value, std::string{"auto"}));
    const FieldRef* baud = find_field(v->fields, "baudRate");
    REQUIRE(baud != nullptr);
    CHECK(same_scalar(baud->default_value, std::int64_t{9600}));
#ifdef ALPACACORE_ENABLE_SYNSCAN
    const DeviceCatalog built = builtin_catalog();
    int number = 10;
    for (const char* spelling : {"auto", "v3", "V3", "3", "v4", "V4", "4", "bogus"}) {
        INFO(spelling);
        DeviceConfig config;
        config.set("connectionType", std::string{"auto"});
        config.set("synscanVersion", std::string{spelling});
        auto driver = built.create(kSynScanKey, config, number);
        REQUIRE(driver != nullptr);
        CHECK(driver->get_device_number() == number++);
    }
#endif
}

// ---------------------------------------------------------------------------
// QHY camera, filter wheel (integrated and CFW3) and Q-Focuser. The CFW3 and
// Q-Focuser auto-detect factories construct without a scan (#659), so no port
// is opened here.
// ---------------------------------------------------------------------------

namespace {

const DeviceKey kQhyCameraKey{"qhy", DeviceType::Camera};
const DeviceKey kQhyWheelKey{"qhy", DeviceType::FilterWheel};
const DeviceKey kQhyFocuserKey{"qhy", DeviceType::Focuser};

}  // namespace

TEST_CASE("Builtin catalog - register_builtin_schemas describes the QHY devices in every build",
          "[catalog][qhy][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    const auto views = catalog.describe();

    const auto keys_of = [](const DescriptorView* v) {
        std::vector<std::string> keys;
        for (const FieldRef& f : v->fields) keys.emplace_back(f.key);
        return keys;
    };
    for (const DeviceKey& key : {kQhyCameraKey, kQhyWheelKey, kQhyFocuserKey}) {
        const DescriptorView* v = find_view(views, key);
        REQUIRE(v != nullptr);
        CHECK(v->build_option == "ALPACACORE_ENABLE_QHY");
        CHECK_FALSE(v->available);  // schemas only: no factory has been registered yet
    }
    CHECK(keys_of(find_view(views, kQhyCameraKey)) == std::vector<std::string>{"cameraIndex", "cameraId"});
    CHECK(keys_of(find_view(views, kQhyWheelKey)) == std::vector<std::string>{"wheelType", "connectionType", "portPath",
                                                                              "filterwheelIndex", "cameraIndex",
                                                                              "cameraId", "filterNames"});
    CHECK(keys_of(find_view(views, kQhyFocuserKey)) ==
          std::vector<std::string>{"connectionType", "portPath", "focuserIndex", "maxStep", "reverse", "speed",
                                   "holdForce", "holdIhold", "holdIrun", "temperatureSource"});

    const DescriptorView* wheel = find_view(views, kQhyWheelKey);
    const FieldRef* names = find_field(wheel->fields, "filterNames");
    REQUIRE(names != nullptr);
    CHECK(names->kind == FieldRef::Kind::StringList);
    const FieldRef* wheel_type = find_field(wheel->fields, "wheelType");
    REQUIRE(wheel_type != nullptr);
    CHECK(wheel_type->allowed_values.empty());  // normalize owns the rule, to keep its wording
    CHECK(same_scalar(wheel_type->default_value, std::string{"integrated"}));
}

TEST_CASE("Builtin catalog - register_builtin_factories makes the QHY devices available only when built",
          "[catalog][qhy][unit]") {
    const DeviceCatalog catalog = builtin_catalog();
    const auto views = catalog.describe();
    for (const DeviceKey& key : {kQhyCameraKey, kQhyWheelKey, kQhyFocuserKey}) {
        const DescriptorView* v = find_view(views, key);
        REQUIRE(v != nullptr);
#ifdef ALPACACORE_ENABLE_QHY
        CHECK(v->available);
#else
        CHECK_FALSE(v->available);
        try {
            (void)catalog.create(key, DeviceConfig{}, 0);
            FAIL("create() must throw when QHY is not built");
        } catch (const std::runtime_error& e) {
            CHECK(std::string(e.what()).find("ALPACACORE_ENABLE_QHY") != std::string::npos);
        }
#endif
    }
#ifdef ALPACACORE_ENABLE_QHY
    // Nothing here touches the SDK or a port: the CFW3 and Q-Focuser auto
    // paths resolve at connect (#659), the camera and integrated wheel open at connect.
    DeviceConfig camera;
    camera.set("cameraIndex", std::int64_t{1});
    auto camera_driver = catalog.create(kQhyCameraKey, camera, 3);
    REQUIRE(camera_driver != nullptr);
    CHECK(camera_driver->get_device_type() == DeviceType::Camera);
    CHECK(camera_driver->get_device_number() == 3);

    DeviceConfig integrated;
    integrated.set("cameraId", std::string{"QHY-TEST-1"});
    integrated.set("filterNames", std::vector<std::string>{"L", "R"});
    auto integrated_driver = catalog.create(kQhyWheelKey, integrated, 4);
    REQUIRE(integrated_driver != nullptr);
    CHECK(integrated_driver->get_device_type() == DeviceType::FilterWheel);

    DeviceConfig cfw3_auto;
    cfw3_auto.set("wheelType", std::string{"cfw3-usb"});
    cfw3_auto.set("connectionType", std::string{"auto"});
    auto cfw3_driver = catalog.create(kQhyWheelKey, cfw3_auto, 5);
    REQUIRE(cfw3_driver != nullptr);
    CHECK_FALSE(cfw3_driver->get_connected());

    DeviceConfig cfw3_serial;
    cfw3_serial.set("wheelType", std::string{"cfw3-usb"});
    cfw3_serial.set("connectionType", std::string{"serial"});
    cfw3_serial.set("portPath", std::string{"/dev/ttyUSB-no-such-cfw3"});
    REQUIRE(catalog.create(kQhyWheelKey, cfw3_serial, 6) != nullptr);

    DeviceConfig focuser_auto;
    auto focuser_driver = catalog.create(kQhyFocuserKey, focuser_auto, 7);
    REQUIRE(focuser_driver != nullptr);
    CHECK(focuser_driver->get_device_type() == DeviceType::Focuser);
    CHECK_FALSE(focuser_driver->get_connected());

    DeviceConfig focuser_serial;
    focuser_serial.set("connectionType", std::string{"serial"});
    focuser_serial.set("portPath", std::string{"/dev/ttyACM-no-such-qfocuser"});
    focuser_serial.set("maxStep", std::int64_t{30000});
    REQUIRE(catalog.create(kQhyFocuserKey, focuser_serial, 8) != nullptr);

    // A saved config normalize could only warn about is refused here, never
    // built into a wheel that would auto-probe (it DTR-resets every CP210x).
    DeviceConfig no_port;
    no_port.set("wheelType", std::string{"cfw3-usb"});
    no_port.set("connectionType", std::string{"serial"});
    CHECK_THROWS_AS(catalog.create(kQhyWheelKey, no_port, 9), AlpacaException);
    CHECK_THROWS_AS(catalog.create(kQhyCameraKey, DeviceConfig{}, 10), AlpacaException);
#endif
}

TEST_CASE("Builtin catalog - QHY camera and integrated wheel need a camera index or id", "[catalog][qhy][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    struct Case {
        DeviceKey key;
        const char* message;
    };
    for (const Case& c : {Case{kQhyCameraKey, "QHY camera requires cameraIndex or cameraId"},
                          Case{kQhyWheelKey, "QHY filter wheel requires cameraIndex or cameraId"}}) {
        INFO(c.message);
        const auto api = catalog.normalize(c.key, DeviceConfig{}, Source::Api);
        REQUIRE(api.rejection.has_value());
        CHECK(*api.rejection == c.message);

        DeviceConfig negative;
        negative.set("cameraIndex", std::int64_t{-1});
        REQUIRE(catalog.normalize(c.key, negative, Source::Api).rejection.has_value());

        // A saved config is warned about and kept.
        const auto persisted = catalog.normalize(c.key, DeviceConfig{}, Source::Persisted);
        CHECK_FALSE(persisted.rejection.has_value());
        REQUIRE(persisted.warnings.size() == 1);
        CHECK(persisted.warnings[0] == c.message);

        DeviceConfig by_index;
        by_index.set("cameraIndex", std::int64_t{0});
        CHECK_FALSE(catalog.normalize(c.key, by_index, Source::Api).rejection.has_value());
        DeviceConfig by_id;
        by_id.set("cameraId", std::string{"QHY-TEST-1"});
        CHECK_FALSE(catalog.normalize(c.key, by_id, Source::Api).rejection.has_value());
    }
}

TEST_CASE("Builtin catalog - QHY CFW3 wheel normalize refuses a bad wheelType, connectionType or endpoint",
          "[catalog][qhy][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    const auto cfw3 = [](const char* type) {
        DeviceConfig config;
        config.set("wheelType", std::string{"cfw3-usb"});
        config.set("connectionType", std::string{type});
        return config;
    };

    DeviceConfig bad_wheel;
    bad_wheel.set("wheelType", std::string{"cfw2-usb"});
    bad_wheel.set("cameraIndex", std::int64_t{0});
    const auto bad_wheel_api = catalog.normalize(kQhyWheelKey, bad_wheel, Source::Api);
    REQUIRE(bad_wheel_api.rejection.has_value());
    CHECK(*bad_wheel_api.rejection == "QHY filter wheel wheelType must be \"integrated\" or \"cfw3-usb\"");

    const auto bad_type = catalog.normalize(kQhyWheelKey, cfw3("network"), Source::Api);
    REQUIRE(bad_type.rejection.has_value());
    CHECK(*bad_type.rejection == "QHY CFW3 connectionType must be \"auto\" or \"serial\"");

    const auto no_port = catalog.normalize(kQhyWheelKey, cfw3("serial"), Source::Api);
    REQUIRE(no_port.rejection.has_value());
    CHECK(*no_port.rejection == "QHY CFW3 connectionType \"serial\" requires portPath");
    const auto no_port_saved = catalog.normalize(kQhyWheelKey, cfw3("serial"), Source::Persisted);
    CHECK_FALSE(no_port_saved.rejection.has_value());
    REQUIRE(no_port_saved.warnings.size() == 1);
    CHECK(no_port_saved.warnings[0] == "QHY CFW3 connectionType \"serial\" requires portPath");

    DeviceConfig negative_index = cfw3("auto");
    negative_index.set("filterwheelIndex", std::int64_t{-1});
    const auto negative = catalog.normalize(kQhyWheelKey, negative_index, Source::Api);
    REQUIRE(negative.rejection.has_value());
    CHECK(*negative.rejection == "QHY CFW3 filterwheelIndex must be 0 or greater");

    // "auto" needs no endpoint; "serial" with a port is fine.
    CHECK_FALSE(catalog.normalize(kQhyWheelKey, cfw3("auto"), Source::Api).rejection.has_value());
    DeviceConfig serial = cfw3("serial");
    serial.set("portPath", std::string{"/dev/ttyUSB7"});
    CHECK_FALSE(catalog.normalize(kQhyWheelKey, serial, Source::Api).rejection.has_value());
}

TEST_CASE("Builtin catalog - QHY Q-Focuser settings are range checked per field", "[catalog][qhy][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    struct Case {
        const char* key;
        std::int64_t value;
    };
    for (const Case& c : {Case{"maxStep", 0}, Case{"maxStep", 2000001}, Case{"speed", 0}, Case{"speed", 9},
                          Case{"holdIhold", 17}, Case{"holdIhold", -1}, Case{"holdIrun", 31}}) {
        INFO(std::string(c.key) + " " + std::to_string(c.value));
        DeviceConfig config;
        config.set(c.key, c.value);
        const auto api = catalog.normalize(kQhyFocuserKey, config, Source::Api);
        REQUIRE(api.rejection.has_value());
        CHECK(api.rejection->find(c.key) != std::string::npos);
        // A saved out-of-range value drops to unset, so the factory's default applies.
        const auto persisted = catalog.normalize(kQhyFocuserKey, config, Source::Persisted);
        CHECK_FALSE(persisted.config.has(c.key));
    }
    for (const Case& c : {Case{"maxStep", 1}, Case{"maxStep", 2000000}, Case{"speed", 8}, Case{"holdIhold", 16},
                          Case{"holdIrun", 30}, Case{"holdIrun", 0}}) {
        INFO(std::string(c.key) + " " + std::to_string(c.value));
        DeviceConfig config;
        config.set(c.key, c.value);
        CHECK_FALSE(catalog.normalize(kQhyFocuserKey, config, Source::Api).rejection.has_value());
    }

    DeviceConfig source;
    source.set("temperatureSource", std::string{"probe"});
    const auto bad_source = catalog.normalize(kQhyFocuserKey, source, Source::Api);
    REQUIRE(bad_source.rejection.has_value());
    CHECK(bad_source.rejection->find("temperatureSource") != std::string::npos);
    source.set("temperatureSource", std::string{"chip"});
    CHECK_FALSE(catalog.normalize(kQhyFocuserKey, source, Source::Api).rejection.has_value());
}

TEST_CASE("Builtin catalog - QHY sanitize keeps the declared fields per device type", "[catalog][qhy][unit]") {
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);

    DeviceConfig focuser;
    focuser.set("connectionType", std::string{"serial"});
    focuser.set("portPath", std::string{"/dev/ttyACM3"});
    focuser.set("focuserIndex", std::int64_t{1});
    focuser.set("maxStep", std::int64_t{30000});
    focuser.set("reverse", true);
    focuser.set("speed", std::int64_t{4});
    focuser.set("holdForce", true);
    focuser.set("holdIhold", std::int64_t{6});
    focuser.set("holdIrun", std::int64_t{12});
    focuser.set("temperatureSource", std::string{"chip"});
    focuser.set("cameraIndex", std::int64_t{7});  // a camera key, not a focuser one
    const DeviceConfig kept = catalog.sanitize(kQhyFocuserKey, focuser);
    CHECK(kept.entries().size() == 10);
    CHECK_FALSE(kept.has("cameraIndex"));
    CHECK(string_at(kept, "portPath") == "/dev/ttyACM3");

    DeviceConfig wheel;
    wheel.set("wheelType", std::string{"cfw3-usb"});
    wheel.set("filterNames", std::vector<std::string>{"L", "R", "G"});
    wheel.set("bogusKey", std::int64_t{1});
    const DeviceConfig kept_wheel = catalog.sanitize(kQhyWheelKey, wheel);
    CHECK(kept_wheel.has("filterNames"));
    CHECK_FALSE(kept_wheel.has("bogusKey"));

    DeviceConfig camera;
    camera.set("cameraIndex", std::int64_t{1});
    camera.set("cameraId", std::string{"QHY-TEST-1"});
    camera.set("portPath", std::string{"/dev/x"});
    const DeviceConfig kept_camera = catalog.sanitize(kQhyCameraKey, camera);
    CHECK(kept_camera.entries().size() == 2);
}

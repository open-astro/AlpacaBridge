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

// The ZWO camera's catalog bind hook over FakeZWOSDK (open-astro#999): the
// duplicate serial / UniqueID checks against the stored entries, the serials
// other entries claim, the UniqueID kept or generated, and what registration
// learns for the entry to store. These are the router arm's rules (#914) moved
// into the catalog; the router-level cases live in AlpacaHTTP/tests/test_routing.cpp.

#include <alpacacore/catalog/builtin_catalog.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../src/catalog/builtin_descriptors.h"
#include "catch2_compat.h"
#include "fake_zwo_sdk.h"

using namespace alpacacore;
using namespace alpacacore::catalog;
using alpacacore::test::FakeZWOSDK;

namespace {

const DeviceKey kCameraKey{"zwo", DeviceType::Camera};
constexpr const char* kFakeSerial = "FAKE-ZWO-17";  // FakeZWOSDK's enumerated camera
constexpr const char* kFakeName = "Fake ZWO Camera";
constexpr const char* kOtherSerial = "0c190e111d020900";

struct Rig {
    FakeZWOSDK sdk;
    DeviceCatalog catalog;
    Rig() {
        register_builtin_schemas(catalog);
        register_zwo_camera_factory(catalog, sdk);
    }
};

DeviceConfig camera_config(std::int64_t index = 0) {
    DeviceConfig config;
    config.set("cameraIndex", index);
    return config;
}

PeerEntry peer(int number, std::optional<std::string> serial, std::optional<std::string> unique_id = std::nullopt) {
    DeviceConfig config = camera_config(number);
    if (serial) config.set("serialNumber", *serial);
    if (unique_id) config.set("uniqueId", *unique_id);
    return {number, std::move(config)};
}

std::optional<std::string> learned(const BindResult& result, const char* key) {
    const ConfigValue* value = result.learned.find_value(key);
    if (!value) return std::nullopt;
    return std::get<std::string>(*value);
}

BindResult bind(Rig& rig, const DeviceConfig& config, int number, Source source, const std::vector<PeerEntry>& peers) {
    return rig.catalog.bind(kCameraKey, config, number, BindContext{source, peers});
}

// Every ZWO line logged at INFO or above while it lives; restores the sink and level on any exit.
struct LogCapture {
    std::vector<std::pair<logging::LogLevel, std::string>> lines;
    logging::LogSink previous_sink = logging::get_log_sink();
    logging::LogLevel previous_level = logging::get_log_level();
    LogCapture() {
        logging::set_log_level(logging::LogLevel::Info);
        logging::set_log_sink([this](logging::LogLevel level, std::string_view component, std::string_view message) {
            if (component == "ZWO") lines.emplace_back(level, std::string(message));
        });
    }
    ~LogCapture() {
        logging::set_log_sink(previous_sink);
        logging::set_log_level(previous_level);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::vector<logging::LogLevel> levels_of(const std::string& prefix) const {
        std::vector<logging::LogLevel> levels;
        for (const auto& [level, message] : lines) {
            if (message.rfind(prefix, 0) == 0) levels.push_back(level);
        }
        return levels;
    }
};

}  // namespace

TEST_CASE("ZWO camera catalog carries USB Limit without persisting an implicit default",
          "[catalog][zwo][camera][unit]") {
    Rig rig;
    auto config = camera_config();
    SECTION("implicit default") {}
    SECTION("explicit") { config.set("usbBandwidth", std::int64_t{65}); }
    const auto normalized = rig.catalog.normalize(kCameraKey, config, Source::Api);
    REQUIRE_FALSE(normalized.rejection);
    const auto saved = rig.catalog.sanitize(kCameraKey, normalized.config);
    CHECK((saved.find_value("usbBandwidth") != nullptr) == (config.find_value("usbBandwidth") != nullptr));
    auto result = bind(rig, saved, 0, Source::Api, {});
    REQUIRE(result.driver);
    result.driver->set_connected(true);
    long value = 0;
    bool is_auto = true;
    REQUIRE(rig.sdk.raw_get_control_value(rig.sdk.camera.camera_id, FakeZWOSDK::ControlType::BandwidthOverload, value,
                                          is_auto));
    CHECK(value == (config.find_value("usbBandwidth") ? 65 : 40));
    CHECK_FALSE(is_auto);
}

TEST_CASE("ZWO camera catalog validates USB Limit and isolates it from switches", "[catalog][zwo][camera][unit]") {
    Rig rig;
    for (const ConfigValue& value : {ConfigValue{std::int64_t{-1}}, ConfigValue{std::int64_t{101}}, ConfigValue{40.5},
                                     ConfigValue{true}, ConfigValue{std::string("40")}}) {
        auto config = camera_config();
        config.set("usbBandwidth", value);
        CHECK(rig.catalog.normalize(kCameraKey, config, Source::Api).rejection.has_value());
        const auto loaded = rig.catalog.normalize(kCameraKey, config, Source::Persisted);
        CHECK_FALSE(loaded.rejection);
        CHECK_FALSE(loaded.warnings.empty());
        CHECK(loaded.config.find_value("usbBandwidth") == nullptr);
    }
    auto config = camera_config();
    config.set("usbBandwidth", std::int64_t{65});
    CHECK(rig.catalog.sanitize({"zwo", DeviceType::Switch}, config).find_value("usbBandwidth") == nullptr);
}

TEST_CASE("ZWO camera catalog - registers a bind-only factory", "[catalog][zwo][camera][unit]") {
    Rig rig;
    CHECK(rig.catalog.has_bind(kCameraKey));
    bool available = false;
    for (const DescriptorView& view : rig.catalog.describe()) {
        if (view.key == kCameraKey) available = view.available;
    }
    CHECK(available);
    // The factory is bind-only: its stored peers and connect hook have no place in create().
    CHECK_THROWS_AS(rig.catalog.create(kCameraKey, camera_config(), 1), std::runtime_error);
}

// #2074: Falsified by: zwo_camera_driver.cpp set_connected() not calling report_identity_learned().
TEST_CASE("ZWO camera catalog - a connect learns the serial and model name, and a second start binds by them (#2074)",
          "[catalog][zwo][camera][unit]") {
    Rig rig;
    std::optional<DeviceConfig> stored;
    const auto store = [&](const DeviceConfig& config) { stored = config; };
    const BindResult result = rig.catalog.bind(kCameraKey, camera_config(0), 3, BindContext{Source::Api, {}, store});
    REQUIRE(result.driver != nullptr);
    CHECK(result.driver->get_device_type() == DeviceType::Camera);
    CHECK(result.driver->get_device_number() == 3);
    CHECK_FALSE(result.rejection.has_value());
    CHECK(result.learned.entries().empty());
    // The UniqueID is the router's stored ID (decision 0018): bind never learns one.
    result.driver->set_connected(true);
    REQUIRE(stored.has_value());
    CHECK(std::get<std::string>(*stored->find_value("serialNumber")) == std::string(kFakeSerial));
    CHECK(std::get<std::string>(*stored->find_value("cameraName")) == std::string(kFakeName));
    CHECK(stored->find_value("uniqueId") == nullptr);
    result.driver->set_connected(false);

    // The second start binds by what the first stored, and reports nothing new.
    stored.reset();
    DeviceConfig known = camera_config(0);
    known.set("serialNumber", std::string(kFakeSerial));
    known.set("cameraName", std::string(kFakeName));
    const BindResult again = rig.catalog.bind(kCameraKey, known, 3, BindContext{Source::Persisted, {}, store});
    REQUIRE(again.driver != nullptr);
    CHECK(again.learned.entries().empty());
    again.driver->set_connected(true);
    CHECK_FALSE(stored.has_value());
    again.driver->set_connected(false);
}

// #2074: opening every matching camera at registration could open one a guider on this host holds.
// Falsified by: zwo_catalog.cpp bind_camera() calling sdk.enumerate_identified_cameras().
TEST_CASE("ZWO camera catalog - registration opens no camera (#2074)", "[catalog][zwo][camera][unit]") {
    Rig rig;
    const BindResult result = bind(rig, camera_config(0), 3, Source::Api, {});
    REQUIRE(result.driver != nullptr);
    CHECK(rig.sdk.call_count("enumerate_identified_cameras") == 0);
    CHECK(rig.sdk.holders(rig.sdk.camera.camera_id) == 0);
}

TEST_CASE("ZWO camera catalog - registers a serial-less camera without learning a UniqueID",
          "[catalog][zwo][camera][unit]") {
    Rig rig;
    rig.sdk.before_call = [](const std::string& call) {
        if (call == "enumerate_identified_cameras") throw AlpacaException("no usb", AlpacaError::DriverException);
    };
    // A camera that is not plugged in still registers with the identity it was configured with.
    const BindResult result = bind(rig, camera_config(1), 4, Source::Persisted, {});
    REQUIRE(result.driver != nullptr);
    CHECK_FALSE(result.rejection.has_value());
    CHECK_FALSE(learned(result, "serialNumber").has_value());
    // Decision 0018: no ZWO_UID_ is generated or stored; the router assigns the GUID.
    CHECK_FALSE(learned(result, "uniqueId").has_value());
}

TEST_CASE("ZWO camera catalog - ignores a uniqueId in the config and in its peers", "[catalog][zwo][camera][unit]") {
    Rig rig;
    rig.sdk.before_call = [](const std::string&) { throw AlpacaException("no usb", AlpacaError::DriverException); };
    // uniqueId is not a ZWO field any more (decision 0018): the same value on two
    // entries is no duplicate for bind, and nothing about it is learned or refused.
    DeviceConfig with_uid = camera_config(1);
    with_uid.set("uniqueId", std::string("ZWO_UID_00112233445566ff"));
    const BindResult result = bind(rig, with_uid, 4, Source::Api, {peer(8, std::nullopt, "ZWO_UID_00112233445566ff")});
    REQUIRE(result.driver != nullptr);
    CHECK_FALSE(result.rejection.has_value());
    CHECK_FALSE(learned(result, "uniqueId").has_value());
}

TEST_CASE("ZWO camera catalog - refuses a serial another entry holds", "[catalog][zwo][camera][unit]") {
    Rig rig;
    DeviceConfig config = camera_config(2);
    config.set("serialNumber", std::string(kOtherSerial));

    const BindResult dup_serial = bind(rig, config, 9, Source::Api, {peer(8, std::string(kOtherSerial))});
    REQUIRE(dup_serial.rejection.has_value());
    CHECK(*dup_serial.rejection == "Invalid value for serialNumber: another ZWO camera already uses it");
    CHECK(dup_serial.driver == nullptr);

    // Its own stored entry is not "another": a re-configure keeps its serial.
    const BindResult own = bind(rig, config, 9, Source::Api, {peer(9, std::string(kOtherSerial))});
    CHECK_FALSE(own.rejection.has_value());
    REQUIRE(own.driver != nullptr);
    // An empty serial is no claim on a camera, even when another entry also has none.
    const BindResult blank = bind(rig, camera_config(2), 9, Source::Api, {peer(8, std::string())});
    CHECK_FALSE(blank.rejection.has_value());
}

TEST_CASE("ZWO camera catalog - a stored duplicate is cleared", "[catalog][zwo][camera][unit]") {
    Rig rig;
    // The stored serial duplicates another entry's: it is dropped, and the clear is handed back as
    // an empty serial, so the entry processed after it keeps the serial.
    DeviceConfig config = camera_config(0);
    config.set("serialNumber", std::string(kOtherSerial));
    const BindResult cleared = bind(rig, config, 9, Source::Persisted, {peer(8, std::string(kOtherSerial))});
    REQUIRE(cleared.driver != nullptr);
    CHECK_FALSE(cleared.rejection.has_value());
    CHECK(learned(cleared, "serialNumber") == std::string());
}

TEST_CASE("ZWO camera catalog - a serial-less entry skips the serials other entries claim",
          "[catalog][zwo][camera][unit]") {
    Rig rig;
    // The only plugged-in camera is the one the other entry is bound to by serial, so this
    // serial-less entry (no selector hint to match) must not take it.
    DeviceConfig by_name;
    by_name.set("cameraName", std::string(kFakeName));
    const BindResult skipped = bind(rig, by_name, 6, Source::Persisted, {peer(5, std::string(kFakeSerial))});
    REQUIRE(skipped.driver != nullptr);
    CHECK_THROWS_AS(skipped.driver->set_connected(true), AlpacaException);

    // Without the claim the same entry binds the camera.
    const BindResult taken = bind(rig, by_name, 6, Source::Persisted, {});
    taken.driver->set_connected(true);
    CHECK(taken.driver->get_connected());
    taken.driver->set_connected(false);
}

// #2074: an entry bound before another learned its serial still must not take that camera.
// Falsified by: zwo_camera_driver.cpp resolve_camera_id() using only binding_.claimed_serials.
TEST_CASE("ZWO camera catalog - a serial learned at connect is claimed from entries already bound (#2074)",
          "[catalog][zwo][camera][unit]") {
    Rig rig;
    rig.sdk.set_units({{3, "1111222233334444"}, {5, "5555666677778888"}});
    DeviceConfig by_name;
    by_name.set("cameraName", std::string(kFakeName));
    const BindResult first = bind(rig, by_name, 1, Source::Persisted, {});
    const BindResult second = bind(rig, by_name, 2, Source::Persisted, {});
    first.driver->set_connected(true);
    second.driver->set_connected(true);
    CHECK(rig.sdk.holders(3) == 1);
    CHECK(rig.sdk.holders(5) == 1);
    first.driver->set_connected(false);
    second.driver->set_connected(false);
}

// #2074: a removed entry must not keep its learned serial away from the entries bound after it.
// Falsified by: zwo_catalog.cpp bind_camera() not calling claims->retain().
TEST_CASE("ZWO camera catalog - a removed entry stops claiming the serial it learned (#2074)",
          "[catalog][zwo][camera][unit]") {
    Rig rig;
    DeviceConfig by_name;
    by_name.set("cameraName", std::string(kFakeName));
    const BindResult first = bind(rig, by_name, 0, Source::Persisted, {});
    first.driver->set_connected(true);
    first.driver->set_connected(false);

    // Entry 0 is still stored, so its serial stays claimed.
    const BindResult kept = bind(rig, by_name, 1, Source::Persisted, {peer(0, std::nullopt)});
    CHECK_THROWS_AS(kept.driver->set_connected(true), AlpacaException);

    // Entry 0 is gone: the next bind frees the camera.
    const BindResult freed = bind(rig, by_name, 1, Source::Persisted, {});
    freed.driver->set_connected(true);
    CHECK(freed.driver->get_connected());
    freed.driver->set_connected(false);
}

TEST_CASE("ZWO camera catalog - a saved serial binds that body whatever cameraIndex says",
          "[catalog][zwo][camera][unit]") {
    // #1970: the web picker saves the picked camera's serial; the index it sends is a hint only.
    Rig rig;
    rig.sdk.set_units({{3, "1111222233334444"}, {5, "5555666677778888"}});
    DeviceConfig config = camera_config(0);
    config.set("serialNumber", std::string("5555666677778888"));
    const BindResult result = bind(rig, config, 2, Source::Api, {});
    REQUIRE(result.driver != nullptr);
    CHECK(result.warnings.empty());
    CHECK_FALSE(learned(result, "serialNumber").has_value());
    CHECK_FALSE(learned(result, "cameraName").has_value());
    result.driver->set_connected(true);
    CHECK(rig.sdk.holders(5) == 1);
    CHECK(rig.sdk.holders(3) == 0);
    result.driver->set_connected(false);
}

// #1969, moved to connect by #2074: registration no longer reads the cameras.
// Falsified by: zwo_camera_driver.cpp resolve_camera_id() not calling describe_unbound_zwo_camera().
TEST_CASE("ZWO camera catalog - an index hint that matches no camera says what is attached at connect (#2074)",
          "[catalog][zwo][camera][unit]") {
    // #1969: three cameras added one at a time, each alone at slot 0, while the form sent index 2.
    Rig rig;
    rig.sdk.set_units({{3, "1111222233334444"}});
    const BindResult result = bind(rig, camera_config(2), 2, Source::Api, {});
    REQUIRE(result.driver != nullptr);
    CHECK_FALSE(result.rejection.has_value());
    CHECK(result.warnings.empty());
    try {
        result.driver->set_connected(true);
        FAIL("Expected the connect to be refused");
    } catch (const AlpacaException& e) {
        CHECK(std::string(e.what()).find("The SDK enumerated slot 0 Fake ZWO Camera, serial 1111222233334444") !=
              std::string::npos);
    }
}

TEST_CASE("ZWO camera catalog - lists the attached cameras for the web picker", "[catalog][zwo][camera][unit]") {
    Rig rig;
    rig.sdk.set_units({{3, "1111222233334444"}, {5, ""}});
    const auto cameras = rig.catalog.list_cameras("zwo");
    REQUIRE(cameras.has_value());
    REQUIRE(cameras->size() == 2);
    CHECK((*cameras)[0].index == 0);
    CHECK((*cameras)[0].name == kFakeName);
    CHECK((*cameras)[0].serial == "1111222233334444");
    CHECK((*cameras)[1].index == 1);
    CHECK((*cameras)[1].serial.empty());
    CHECK_FALSE(rig.catalog.list_cameras("qhy").has_value());
}

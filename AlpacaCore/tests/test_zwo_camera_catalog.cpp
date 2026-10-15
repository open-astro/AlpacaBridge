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

#include <optional>
#include <string>
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

}  // namespace

TEST_CASE("ZWO camera catalog - registers a bind-only factory", "[catalog][zwo][camera][unit]") {
    Rig rig;
    CHECK(rig.catalog.has_bind(kCameraKey));
    bool available = false;
    for (const DescriptorView& view : rig.catalog.describe()) {
        if (view.key == kCameraKey) available = view.available;
    }
    CHECK(available);
    // Registration enumerates the SDK, so there is no hardware-free create.
    CHECK_THROWS_AS(rig.catalog.create(kCameraKey, camera_config(), 1), std::runtime_error);
}

TEST_CASE("ZWO camera catalog - learns the serial and model name from the camera it binds",
          "[catalog][zwo][camera][unit]") {
    Rig rig;
    const BindResult result = bind(rig, camera_config(0), 3, Source::Api, {});
    REQUIRE(result.driver != nullptr);
    CHECK(result.driver->get_device_type() == DeviceType::Camera);
    CHECK(result.driver->get_device_number() == 3);
    CHECK_FALSE(result.rejection.has_value());
    CHECK(learned(result, "serialNumber") == std::string(kFakeSerial));
    CHECK(learned(result, "cameraName") == std::string(kFakeName));
    // A body that reports a serial needs no generated UniqueID.
    CHECK_FALSE(learned(result, "uniqueId").has_value());
    CHECK(rig.sdk.call_count("enumerate_identified_cameras") == 1);

    // Nothing is learned that the entry already holds.
    DeviceConfig known = camera_config(0);
    known.set("serialNumber", std::string(kFakeSerial));
    known.set("cameraName", std::string(kFakeName));
    const BindResult again = bind(rig, known, 3, Source::Persisted, {});
    REQUIRE(again.driver != nullptr);
    CHECK(again.learned.entries().empty());
}

TEST_CASE("ZWO camera catalog - generates a UniqueID when no serial is known and the SDK fails",
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
    const auto unique_id = learned(result, "uniqueId");
    REQUIRE(unique_id.has_value());
    CHECK(unique_id->rfind("ZWO_UID_", 0) == 0);
}

TEST_CASE("ZWO camera catalog - keeps the UniqueID stored for the same device number", "[catalog][zwo][camera][unit]") {
    Rig rig;
    rig.sdk.before_call = [](const std::string&) { throw AlpacaException("no usb", AlpacaError::DriverException); };
    // A re-configure that does not resend uniqueId keeps the one stored for this number,
    // and hands it back so the entry stores it again.
    const BindResult kept =
        bind(rig, camera_config(1), 4, Source::Api, {peer(4, std::nullopt, "ZWO_UID_0011223344556677")});
    REQUIRE(kept.driver != nullptr);
    CHECK(learned(kept, "uniqueId") == std::string("ZWO_UID_0011223344556677"));

    // A stored id with a control byte is not trusted: a new one is generated.
    const BindResult forged =
        bind(rig, camera_config(1), 4, Source::Persisted, {peer(4, std::nullopt, "ZWO_UID_\n77")});
    const auto regenerated = learned(forged, "uniqueId");
    REQUIRE(regenerated.has_value());
    CHECK(regenerated->rfind("ZWO_UID_", 0) == 0);
    CHECK(*regenerated != "ZWO_UID_\n77");

    // The id the client supplies wins over the stored one and is not "learned".
    DeviceConfig supplied = camera_config(1);
    supplied.set("uniqueId", std::string("ZWO_UID_aaaaaaaaaaaaaaaa"));
    const BindResult mine = bind(rig, supplied, 4, Source::Api, {peer(4, std::nullopt, "ZWO_UID_0011223344556677")});
    REQUIRE(mine.driver != nullptr);
    CHECK_FALSE(learned(mine, "uniqueId").has_value());
}

TEST_CASE("ZWO camera catalog - refuses a serial or UniqueID another entry holds", "[catalog][zwo][camera][unit]") {
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

    DeviceConfig with_uid = camera_config(2);
    with_uid.set("uniqueId", std::string("ZWO_UID_00112233445566ff"));
    const BindResult dup_uid = bind(rig, with_uid, 9, Source::Api, {peer(8, std::nullopt, "ZWO_UID_00112233445566ff")});
    REQUIRE(dup_uid.rejection.has_value());
    CHECK(*dup_uid.rejection == "Invalid value for uniqueId: another ZWO camera already uses it");
}

TEST_CASE("ZWO camera catalog - a stored duplicate is cleared and relearned", "[catalog][zwo][camera][unit]") {
    Rig rig;
    // The stored serial duplicates another entry's: it is dropped, and the camera the
    // index hint names (FakeZWOSDK's, with its own serial) is relearned in its place.
    DeviceConfig config = camera_config(0);
    config.set("serialNumber", std::string(kOtherSerial));
    const BindResult relearned = bind(rig, config, 9, Source::Persisted, {peer(8, std::string(kOtherSerial))});
    REQUIRE(relearned.driver != nullptr);
    CHECK_FALSE(relearned.rejection.has_value());
    CHECK(learned(relearned, "serialNumber") == std::string(kFakeSerial));

    // Nothing to relearn (the model named does not match what is plugged in): the clear is
    // handed back as an empty serial, so the entry processed after it keeps the serial.
    DeviceConfig absent = camera_config(0);
    absent.set("serialNumber", std::string(kOtherSerial));
    absent.set("cameraName", "ZWO Not Plugged In");
    const BindResult cleared = bind(rig, absent, 9, Source::Persisted, {peer(8, std::string(kOtherSerial))});
    REQUIRE(cleared.driver != nullptr);
    CHECK(learned(cleared, "serialNumber") == std::string());

    // A duplicated UniqueID is cleared on its own and a new one generated.
    DeviceConfig uid = camera_config(0);
    uid.set("uniqueId", std::string("ZWO_UID_00112233445566ff"));
    const BindResult fresh = bind(rig, uid, 9, Source::Persisted, {peer(8, std::nullopt, "ZWO_UID_00112233445566ff")});
    REQUIRE(fresh.driver != nullptr);
    CHECK_FALSE(fresh.rejection.has_value());
    const auto new_uid = learned(fresh, "uniqueId");
    // The camera has a serial, so no UniqueID is generated: the cleared one is simply not kept.
    CHECK_FALSE(new_uid.has_value());
    CHECK(learned(fresh, "serialNumber") == std::string(kFakeSerial));
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
    CHECK_FALSE(learned(skipped, "serialNumber").has_value());
    const auto generated = learned(skipped, "uniqueId");
    REQUIRE(generated.has_value());
    CHECK(generated->rfind("ZWO_UID_", 0) == 0);

    // Without the claim the same entry binds the camera and learns its serial.
    const BindResult taken = bind(rig, by_name, 6, Source::Persisted, {});
    CHECK(learned(taken, "serialNumber") == std::string(kFakeSerial));
}

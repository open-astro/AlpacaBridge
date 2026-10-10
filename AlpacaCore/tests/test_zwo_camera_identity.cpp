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

#include <alpacacore/vendor/zwo/zwo_camera_identity.h>

#include <set>
#include <string>
#include <vector>

#include "catch2_compat.h"

using alpacacore::vendor::zwo::resolve_zwo_camera;
using alpacacore::vendor::zwo::ZwoConfiguredIdentity;
using alpacacore::vendor::zwo::ZwoEnumeratedCamera;
using alpacacore::vendor::zwo::ZwoResolveFailure;

namespace {

const char* const kProSerial = "0c190e111d020900";

ZwoEnumeratedCamera pro(int index) { return {index, 100 + index, "ZWO ASI533MC Pro", kProSerial}; }

ZwoEnumeratedCamera mini(int index) { return {index, 100 + index, "ZWO ASI120MM Mini", ""}; }

ZwoConfiguredIdentity by_serial(const std::string& serial, int index_hint) {
    ZwoConfiguredIdentity entry;
    entry.serial = serial;
    entry.camera_index = index_hint;
    return entry;
}

ZwoConfiguredIdentity by_name(const std::string& name, int index_hint) {
    ZwoConfiguredIdentity entry;
    entry.camera_name = name;
    entry.camera_index = index_hint;
    return entry;
}

}  // namespace

TEST_CASE("ZWO camera identity - serial binds the same camera when enumeration order swaps",
          "[zwo][camera][identity][unit]") {
    const auto entry = by_serial(kProSerial, 0);
    const std::set<std::string> none;

    const auto first_start = resolve_zwo_camera(entry, {pro(0), mini(1)}, none);
    const auto second_start = resolve_zwo_camera(entry, {mini(0), pro(1)}, none);

    REQUIRE(first_start.camera.has_value());
    REQUIRE(second_start.camera.has_value());
    CHECK(first_start.camera->serial == kProSerial);
    CHECK(second_start.camera->serial == kProSerial);
    CHECK(first_start.camera->camera_id == 100);
    CHECK(second_start.camera->camera_id == 101);
}

TEST_CASE("ZWO camera identity - a serial whose camera is absent never binds another camera",
          "[zwo][camera][identity][unit]") {
    const auto result = resolve_zwo_camera(by_serial("deadbeef00000000", 0), {pro(0), mini(1)}, {});

    CHECK_FALSE(result.camera.has_value());
    CHECK(result.failure == ZwoResolveFailure::NotFound);
    CHECK(result.message == "ZWO camera serial deadbeef00000000 not found");
}

TEST_CASE("ZWO camera identity - a hint index pointing at another serial is not followed",
          "[zwo][camera][identity][unit]") {
    // Index 0 now holds the Pro; the entry's serial belongs to a different body.
    const auto result = resolve_zwo_camera(by_serial("1111222233334444", 0), {pro(0)}, {});

    CHECK_FALSE(result.camera.has_value());
}

TEST_CASE("ZWO camera identity - serial-less entry binds by name and skips a claimed serial",
          "[zwo][camera][identity][unit]") {
    const auto entry = by_name("ZWO ASI120MM Mini", 1);
    const std::set<std::string> claimed{kProSerial};

    const auto swapped = resolve_zwo_camera(entry, {mini(0), pro(1)}, claimed);
    const auto original = resolve_zwo_camera(entry, {pro(0), mini(1)}, claimed);

    REQUIRE(swapped.camera.has_value());
    REQUIRE(original.camera.has_value());
    CHECK(swapped.camera->name == "ZWO ASI120MM Mini");
    CHECK(original.camera->name == "ZWO ASI120MM Mini");

    // A serial-less entry named after a model whose only body is claimed by
    // another entry binds nothing.
    const auto taken = resolve_zwo_camera(by_name("ZWO ASI533MC Pro", 0), {pro(0), mini(1)}, claimed);
    CHECK_FALSE(taken.camera.has_value());
    CHECK(taken.message == "ZWO camera ZWO ASI533MC Pro not found");
}

TEST_CASE("ZWO camera identity - name match ignores the SDK's trailing padding", "[zwo][camera][identity][unit]") {
    auto padded = mini(0);
    padded.name = std::string("ZWO ASI120MM Mini") + "   " + std::string(2, '\0');

    const auto result = resolve_zwo_camera(by_name("ZWO ASI120MM Mini  ", 5), {padded}, {});

    CHECK(result.camera.has_value());
    CHECK(alpacacore::vendor::zwo::trim_zwo_name(padded.name) == "ZWO ASI120MM Mini");
}

TEST_CASE("ZWO camera identity - same-model serial-less bodies are told apart by the index hint",
          "[zwo][camera][identity][unit]") {
    const std::vector<ZwoEnumeratedCamera> found{mini(0), mini(1)};

    const auto second = resolve_zwo_camera(by_name("ZWO ASI120MM Mini", 1), found, {});
    const auto first = resolve_zwo_camera(by_name("ZWO ASI120MM Mini", 0), found, {});

    REQUIRE(second.camera.has_value());
    REQUIRE(first.camera.has_value());
    CHECK(second.camera->index == 1);
    CHECK(first.camera->index == 0);
}

TEST_CASE("ZWO camera identity - an entry without serial or name binds by id, then index",
          "[zwo][camera][identity][unit]") {
    ZwoConfiguredIdentity by_id;
    by_id.camera_id = 101;
    ZwoConfiguredIdentity by_index;
    by_index.camera_index = 1;
    ZwoConfiguredIdentity out_of_range;
    out_of_range.camera_index = 7;

    const std::vector<ZwoEnumeratedCamera> found{pro(0), mini(1)};
    CHECK(resolve_zwo_camera(by_id, found, {}).camera->camera_id == 101);
    CHECK(resolve_zwo_camera(by_index, found, {}).camera->camera_id == 101);
    CHECK(resolve_zwo_camera(out_of_range, found, {}).failure == ZwoResolveFailure::IndexOutOfRange);
    CHECK(resolve_zwo_camera(by_index, {}, {}).failure == ZwoResolveFailure::NoCameras);
}

TEST_CASE("ZWO camera identity - an id or index hint never binds a camera another entry claims",
          "[zwo][camera][identity][unit]") {
    // An entry re-added by the web form without its serial, after the
    // enumeration order flipped: the hint points at the other entry's camera.
    ZwoConfiguredIdentity by_id;
    by_id.camera_id = 101;
    by_id.camera_index = 1;
    ZwoConfiguredIdentity by_index;
    by_index.camera_index = 1;

    const std::vector<ZwoEnumeratedCamera> found{mini(0), pro(1)};
    const std::set<std::string> claimed{kProSerial};
    const auto id_result = resolve_zwo_camera(by_id, found, claimed);
    CHECK_FALSE(id_result.camera.has_value());
    CHECK(id_result.failure == ZwoResolveFailure::NotFound);
    const auto index_result = resolve_zwo_camera(by_index, found, claimed);
    CHECK_FALSE(index_result.camera.has_value());
    CHECK(index_result.failure == ZwoResolveFailure::NotFound);
    // Unclaimed, the same hints still bind.
    CHECK(resolve_zwo_camera(by_id, found, {}).camera->index == 1);
    CHECK(resolve_zwo_camera(by_index, found, {}).camera->index == 1);
}

TEST_CASE("ZWO camera identity - UniqueID", "[zwo][camera][identity][unit]") {
    using alpacacore::vendor::zwo::generate_zwo_unique_id;
    using alpacacore::vendor::zwo::zwo_unique_id;

    const std::string generated = generate_zwo_unique_id();
    CHECK(generated.rfind("ZWO_UID_", 0) == 0);
    CHECK(generated.size() == std::string("ZWO_UID_").size() + 16);
    CHECK(generated.find_first_not_of("0123456789abcdef", 8) == std::string::npos);
    CHECK(generate_zwo_unique_id() != generated);

    CHECK(zwo_unique_id(kProSerial, generated) == std::string("ZWO_SN_") + kProSerial);
    // Serial-less: the stored value, the same on every start, never an index.
    CHECK(zwo_unique_id("", generated) == generated);
    CHECK(zwo_unique_id("", generated) == zwo_unique_id("", generated));
}

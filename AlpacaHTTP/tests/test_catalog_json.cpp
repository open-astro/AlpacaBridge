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

// The JSON bridge of the device catalog (open-astro#664, Part A), over the
// test descriptor in test_catalog_descriptor.h. Hand-rolled EXPECT tests,
// like the other AlpacaHTTP suites. No vendor header, no hardware.
//
// Contracts pinned here (each is stated in core/catalog_json.h):
//   1. config_from_json converts only declared keys; undeclared keys drop.
//   2. A JSON null or an absent key produces no entry, inside records too (#388).
//   3. A value of the wrong JSON kind throws AlpacaException(InvalidValue) whose
//      message names the field the way config_get() does; a nested field is
//      named with its index, "ports[1].name". The check is on the JSON kind,
//      not on nlohmann's get<>(): a bool converts to int64 there, so `true` in
//      an int field must still be refused.
//   4. A JSON integer in a double field is accepted and stored as the integer;
//      DeviceCatalog::normalize widens it, so the round trip stays exact.
//   5. json_from_config(config_from_json(j)) == j restricted to declared,
//      non-null keys.
//   6. describe_json emits exactly the Part B shape, ordered by vendor then
//      device type, integer range bounds for int fields, and `default` only
//      where the field has one (scalars always; lists only when non-empty).

#include <alpacacore/catalog/device_catalog.h>
#include <alpacacore/util/error_handling.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "core/catalog_json.h"
#include "test_assert.h"
#include "test_catalog_descriptor.h"

namespace {

namespace cj = alpacahttp::catalog_json;
namespace tc = alpacahttp::test_catalog;
// AlpacaError is a namespace of int constants, not a type: a using-declaration
// cannot name it (unlike AlpacaException below), only a namespace alias can.
namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using alpacacore::catalog::DeviceCatalog;
using alpacacore::catalog::DeviceConfig;
using alpacacore::catalog::Source;

// Runs the bridge over `text` and returns the InvalidValue message, or "" when
// nothing was thrown (which the caller EXPECTs against).
std::string invalid_value_message(const std::string& text) {
    try {
        (void)cj::config_from_json(nlohmann::json::parse(text), tc::make_schema());
    } catch (const AlpacaException& e) {
        EXPECT(e.error_code() == AlpacaError::InvalidValue);
        return e.what();
    }
    return "";
}

std::set<std::string> keys_of(const nlohmann::json& object) {
    std::set<std::string> keys;
    for (auto it = object.begin(); it != object.end(); ++it) keys.insert(it.key());
    return keys;
}

const nlohmann::json& field_named(const nlohmann::json& descriptor, const std::string& key) {
    for (const auto& f : descriptor.at("fields")) {
        if (f.at("key") == key) return f;
    }
    static const nlohmann::json none;
    return none;
}

}  // namespace

int main() {
    const auto schema = tc::make_schema();

    // 2. null and absent keys produce no entry, including ports[1].name.
    {
        const auto j = nlohmann::json::parse(
            R"({"enabled":true,"count":null,"ports":[{"name":"p0","pwm":null},{"name":null,"pwm":true}]})");
        const DeviceConfig c = cj::config_from_json(j, schema);
        EXPECT(c.has("enabled"));
        EXPECT(!c.has("count"));
        EXPECT(!c.has("ratio"));
        const auto ports = c.find(tc::ports_field());
        EXPECT(ports.has_value() && ports->size() == 2);
        EXPECT((*ports)[0].has("name") && !(*ports)[0].has("pwm"));
        EXPECT(!(*ports)[1].has("name") && (*ports)[1].has("pwm"));
        // An empty record list is an entry (has() true), not an absence.
        const DeviceConfig e = cj::config_from_json(nlohmann::json::parse(R"({"ports":[]})"), schema);
        EXPECT(e.has("ports"));
    }

    // 3. Wrong JSON kind: InvalidValue, naming the field.
    {
        EXPECT(invalid_value_message(R"({"count":"seven"})") ==
               "Device config field 'count' has the wrong type (got string)");
        EXPECT(invalid_value_message(R"({"enabled":1})").find("enabled") != std::string::npos);
        EXPECT(invalid_value_message(R"({"count":true})").find("count") != std::string::npos);
        EXPECT(invalid_value_message(R"({"count":1.5})").find("count") != std::string::npos);
        EXPECT(invalid_value_message(R"({"mode":7})").find("mode") != std::string::npos);
        EXPECT(invalid_value_message(R"({"filterNames":"L,R"})").find("filterNames") != std::string::npos);
        EXPECT(invalid_value_message(R"({"filterNames":["L",2]})").find("filterNames") != std::string::npos);
        EXPECT(invalid_value_message(R"({"ports":{"name":"p0"}})").find("ports") != std::string::npos);
        EXPECT(invalid_value_message(R"({"ports":[{"name":"p0"},"p1"]})").find("ports") != std::string::npos);
        EXPECT(invalid_value_message(R"({"ports":[{"name":"p0"},{"name":5}]})").find("ports[1].name") !=
               std::string::npos);
        // The message is about the type, not about validation: an out-of-range
        // or out-of-enum value is the catalog's business and converts cleanly.
        EXPECT(invalid_value_message(R"({"count":99,"mode":"zzz"})").empty());
    }

    // 3a. A whole-valued JSON float in an int field (e.g. 1.0) is accepted and
    // stored as the integer, as config_get<int>() accepted it; a fractional or
    // out-of-int64-range float is still the wrong type.
    {
        const DeviceConfig c = cj::config_from_json(nlohmann::json::parse(R"({"count":2.0})"), schema);
        const auto* v = c.find_value("count");
        EXPECT(v != nullptr && std::holds_alternative<std::int64_t>(*v) && std::get<std::int64_t>(*v) == 2);
        EXPECT(invalid_value_message(R"({"count":-3.0})").empty());
        EXPECT(invalid_value_message(R"({"count":1e300})").find("count") != std::string::npos);
    }

    // 4. An integer in a double field is accepted; normalize widens it.
    {
        const DeviceConfig c = cj::config_from_json(nlohmann::json::parse(R"({"ratio":2})"), schema);
        const auto* v = c.find_value("ratio");
        EXPECT(v != nullptr && std::holds_alternative<std::int64_t>(*v) && std::get<std::int64_t>(*v) == 2);
        DeviceCatalog catalog;
        tc::add_schema(catalog);
        const auto r = catalog.normalize(tc::kKey, c, Source::Api);
        EXPECT(!r.rejection.has_value());
        EXPECT(r.config.find(tc::kRatio).has_value() && *r.config.find(tc::kRatio) == 2.0);
        // A JSON double in a double field is stored as the double.
        const DeviceConfig d = cj::config_from_json(nlohmann::json::parse(R"({"ratio":0.75})"), schema);
        EXPECT(d.find(tc::kRatio).has_value() && *d.find(tc::kRatio) == 0.75);
    }

    // 1. Undeclared keys drop; declared keys keep their kinds.
    {
        const auto j = nlohmann::json::parse(
            R"({"vendor":"zzz","deviceType":"focuser","deviceNumber":3,"junk":1,"count":4,"mode":"b","filterNames":["L","R"]})");
        const DeviceConfig c = cj::config_from_json(j, schema);
        EXPECT(!c.has("vendor") && !c.has("deviceType") && !c.has("deviceNumber") && !c.has("junk"));
        EXPECT(c.get(tc::kCount) == 4);
        EXPECT(c.get(tc::kMode) == "b");
        const std::vector<std::string> names{"L", "R"};
        EXPECT(c.get(tc::kFilterNames) == names);
        EXPECT(c.entries().size() == 3);
    }

    // 5. Round trip: declared, non-null keys survive exactly; nothing else does.
    {
        const auto j = nlohmann::json::parse(
            R"({"enabled":false,"count":3,"ratio":0.75,"mode":"b","host":"h","token":"t","filterNames":["L"],)"
            R"("ports":[{"name":"p0","pwm":true},{"name":"p1","pwm":null}],"junk":null,"extra":1,"absent":null})");
        const auto expected = nlohmann::json::parse(
            R"({"enabled":false,"count":3,"ratio":0.75,"mode":"b","host":"h","token":"t","filterNames":["L"],)"
            R"("ports":[{"name":"p0","pwm":true},{"name":"p1"}]})");
        const nlohmann::json back = cj::json_from_config(cj::config_from_json(j, schema));
        EXPECT(back == expected);
        EXPECT(back.dump() == expected.dump());
        // The integer-in-a-double-field case round-trips as the integer it was.
        const auto i = nlohmann::json::parse(R"({"ratio":2})");
        EXPECT(cj::json_from_config(cj::config_from_json(i, schema)).dump() == i.dump());
        // Empty config in, empty object out.
        EXPECT(cj::json_from_config(DeviceConfig{}).dump() == "{}");
    }

    // 6. describe_json: the Part B shape, exactly.
    {
        DeviceCatalog catalog;
        tc::add_schema(catalog);
        // A second descriptor with a vendor that sorts first, added last, to
        // pin "ordered by vendor then device type" rather than insertion order.
        alpacacore::catalog::Schema first = tc::make_schema();
        first.key = alpacacore::catalog::DeviceKey{"aaa", alpacacore::DeviceType::Switch};
        first.display_name = "First Switch";
        first.build_option = "ALPACACORE_ENABLE_AAA";
        catalog.add(first);

        nlohmann::json described = cj::describe_json(catalog);
        EXPECT(described.is_array() && described.size() == 2);
        EXPECT(described[0].at("vendor") == "aaa" && described[0].at("deviceType") == "switch");
        EXPECT(described[1].at("vendor") == "zzz" && described[1].at("deviceType") == "focuser");

        const nlohmann::json& zzz = described[1];
        const std::set<std::string> descriptor_keys{"vendor",    "deviceType",  "displayName",
                                                    "available", "buildOption", "fields"};
        EXPECT(keys_of(zzz) == descriptor_keys);
        EXPECT(zzz.at("displayName") == "Test Focuser");
        EXPECT(zzz.at("buildOption") == "ALPACACORE_ENABLE_ZZZ");
        EXPECT(zzz.at("available").is_boolean() && zzz.at("available") == false);
        EXPECT(zzz.at("fields").is_array() && zzz.at("fields").size() == 8);
        // Declaration order inside a descriptor.
        EXPECT(zzz.at("fields")[0].at("key") == "enabled" && zzz.at("fields")[7].at("key") == "ports");

        const auto& count = field_named(zzz, "count");
        const std::set<std::string> count_keys{"key", "type", "default", "required", "secret", "role", "range"};
        EXPECT(keys_of(count) == count_keys);
        EXPECT(count.at("type") == "int" && count.at("role") == "Plain");
        EXPECT(count.at("default").is_number_integer() && count.at("default") == 1);
        EXPECT(count.at("required") == false && count.at("secret") == false);
        // Integer bounds for an int field: "1", not "1.0".
        EXPECT(count.at("range").dump() == R"({"max":8,"min":1})");

        const auto& ratio = field_named(zzz, "ratio");
        EXPECT(ratio.at("type") == "double" && ratio.at("default") == 1.5);
        EXPECT(ratio.at("range").dump() == R"({"min":0.5})");

        const auto& enabled = field_named(zzz, "enabled");
        EXPECT(enabled.at("type") == "bool" && enabled.at("default").is_boolean() && enabled.at("default") == false);
        EXPECT(!enabled.contains("range") && !enabled.contains("enum") && !enabled.contains("appliesWhen") &&
               !enabled.contains("fields"));

        const auto& mode = field_named(zzz, "mode");
        EXPECT(mode.at("type") == "string" && mode.at("role") == "Discriminator" && mode.at("default") == "a");
        EXPECT(mode.at("enum").dump() == R"(["a","b"])");

        const auto& host = field_named(zzz, "host");
        EXPECT(host.at("role") == "Host" && host.at("default") == "");
        EXPECT(host.at("appliesWhen").dump() == R"({"key":"mode","value":"b"})");

        const auto& token = field_named(zzz, "token");
        EXPECT(token.at("role") == "Secret" && token.at("secret") == true);

        const auto& names = field_named(zzz, "filterNames");
        EXPECT(names.at("type") == "stringList" && !names.contains("default") && !names.contains("fields"));

        const auto& ports = field_named(zzz, "ports");
        EXPECT(ports.at("type") == "recordList" && !ports.contains("default"));
        EXPECT(ports.at("fields").is_array() && ports.at("fields").size() == 2);
        const auto& name = ports.at("fields")[0];
        EXPECT(name.at("key") == "name" && name.at("type") == "string" && name.at("required") == true);
        EXPECT((keys_of(name) == std::set<std::string>{"key", "type", "default", "required", "secret", "role"}));
        EXPECT(ports.at("fields")[1].at("key") == "pwm" && ports.at("fields")[1].at("type") == "bool");

        // A factory flips `available` and nothing else. A fresh variable, not a
        // reassignment of `described`, because `zzz` is a reference into its old
        // content: reassigning through operator= would invalidate it before the
        // comparison below reads it.
        catalog.add(tc::make_factory());
        const nlohmann::json described_with_factory = cj::describe_json(catalog);
        EXPECT(described_with_factory[1].at("available") == true);
        EXPECT(described_with_factory[1].at("fields") == zzz.at("fields"));
    }

    // 6. An int64 extreme bound: Field::ref() widens INT64_MAX to the double 2^63,
    // which no int64 holds, and the range must still read INT64_MAX.
    {
        static const alpacacore::catalog::Field<std::int64_t> kBig{
            .key = "big", .default_value = 0, .min = 0, .max = std::numeric_limits<std::int64_t>::max()};
        static const std::vector<alpacacore::catalog::FieldRef> big_fields{kBig.ref()};
        alpacacore::catalog::Schema big = tc::make_schema();
        big.fields = big_fields;
        DeviceCatalog catalog;
        catalog.add(big);
        const nlohmann::json described = cj::describe_json(catalog);
        EXPECT(field_named(described[0], "big").at("range").dump() == R"({"max":9223372036854775807,"min":0})");
    }

    std::cout << "All catalog JSON bridge tests passed!\n";
    return 0;
}

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

#include "catalog_json.h"

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/util/error_handling.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace alpacahttp::catalog_json {

namespace {

using alpacacore::AlpacaException;
using alpacacore::catalog::ConfigValue;
using alpacacore::catalog::DeviceConfig;
using alpacacore::catalog::FieldRef;
using alpacacore::catalog::int_bound;
using alpacacore::catalog::Role;

[[noreturn]] void throw_wrong_type(const std::string& name, const nlohmann::json& value) {
    throw AlpacaException(
        std::string("Device config field '") + name + "' has the wrong type (got " + value.type_name() + ")",
        alpacacore::AlpacaError::InvalidValue);
}

// A JSON float with no fractional part that fits an int64 (e.g. 1.0), as the
// integer it holds; config_get<int>() accepted that shape. Anything else,
// including 1.5 and a JSON true/false (which config_get<int>() took as 1/0),
// is not a whole number here.
std::optional<std::int64_t> whole_number(const nlohmann::json& v) {
    if (!v.is_number_float()) return std::nullopt;
    const double d = v.get<double>();
    // 2^63 is exact as a double; the int64 range is [-2^63, 2^63).
    constexpr double kTwo63 = 9223372036854775808.0;
    if (!(d >= -kTwo63 && d < kTwo63) || d != std::trunc(d)) return std::nullopt;
    return static_cast<std::int64_t>(d);
}

DeviceConfig config_from_json_fields(const nlohmann::json& object, std::span<const FieldRef> fields,
                                     const std::string& prefix) {
    DeviceConfig out;
    for (const FieldRef& f : fields) {
        const std::string name = prefix + f.key;
        const auto it = object.find(f.key);
        if (it == object.end() || it->is_null()) continue;  // absent or null: no entry (#388)
        const nlohmann::json& v = *it;
        switch (f.kind) {
            case FieldRef::Kind::Bool:
                if (!v.is_boolean()) throw_wrong_type(name, v);
                out.set(f.key, v.get<bool>());
                break;
            case FieldRef::Kind::Int:
                if (v.is_number_integer()) {
                    out.set(f.key, v.get<std::int64_t>());
                } else if (const auto whole = whole_number(v)) {
                    out.set(f.key, *whole);
                } else {
                    throw_wrong_type(name, v);
                }
                break;
            case FieldRef::Kind::Double:
                if (!v.is_number()) throw_wrong_type(name, v);
                if (v.is_number_integer())
                    out.set(f.key, v.get<std::int64_t>());
                else
                    out.set(f.key, v.get<double>());
                break;
            case FieldRef::Kind::String:
                if (!v.is_string()) throw_wrong_type(name, v);
                out.set(f.key, v.get<std::string>());
                break;
            case FieldRef::Kind::StringList: {
                if (!v.is_array()) throw_wrong_type(name, v);
                std::vector<std::string> list;
                list.reserve(v.size());
                for (const auto& elem : v) {
                    if (!elem.is_string()) throw_wrong_type(name, elem);
                    list.push_back(elem.get<std::string>());
                }
                out.set(f.key, std::move(list));
                break;
            }
            case FieldRef::Kind::RecordList: {
                if (!v.is_array()) throw_wrong_type(name, v);
                std::vector<DeviceConfig> records;
                records.reserve(v.size());
                for (std::size_t i = 0; i < v.size(); ++i) {
                    const nlohmann::json& elem = v[i];
                    if (!elem.is_object()) throw_wrong_type(name, elem);
                    records.push_back(
                        config_from_json_fields(elem, f.record_fields, name + "[" + std::to_string(i) + "]."));
                }
                out.set(f.key, std::move(records));
                break;
            }
        }
    }
    return out;
}

nlohmann::json value_to_json(const ConfigValue& value) {
    return std::visit(
        [](const auto& held) -> nlohmann::json {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, std::vector<DeviceConfig>>) {
                nlohmann::json arr = nlohmann::json::array();
                for (const DeviceConfig& record : held) arr.push_back(json_from_config(record));
                return arr;
            } else {
                return nlohmann::json(held);
            }
        },
        value);
}

const char* kind_to_string(FieldRef::Kind kind) {
    switch (kind) {
        case FieldRef::Kind::Bool:
            return "bool";
        case FieldRef::Kind::Int:
            return "int";
        case FieldRef::Kind::Double:
            return "double";
        case FieldRef::Kind::String:
            return "string";
        case FieldRef::Kind::StringList:
            return "stringList";
        case FieldRef::Kind::RecordList:
            return "recordList";
    }
    return "string";
}

const char* role_to_string(Role role) {
    switch (role) {
        case Role::Plain:
            return "Plain";
        case Role::EnumerationIndex:
            return "EnumerationIndex";
        case Role::DeviceId:
            return "DeviceId";
        case Role::PortPath:
            return "PortPath";
        case Role::Host:
            return "Host";
        case Role::Secret:
            return "Secret";
        case Role::Discriminator:
            return "Discriminator";
    }
    return "Plain";
}

bool default_is_empty_list(const FieldRef& f) {
    if (f.kind == FieldRef::Kind::StringList) {
        return std::get<std::vector<std::string>>(f.default_value).empty();
    }
    if (f.kind == FieldRef::Kind::RecordList) {
        return std::get<std::vector<DeviceConfig>>(f.default_value).empty();
    }
    return false;
}

nlohmann::json field_to_json(const FieldRef& f) {
    nlohmann::json j;
    j["key"] = f.key;
    j["type"] = kind_to_string(f.kind);
    const bool is_list = f.kind == FieldRef::Kind::StringList || f.kind == FieldRef::Kind::RecordList;
    if (!is_list || !default_is_empty_list(f)) {
        j["default"] = value_to_json(f.default_value);
    }
    j["required"] = f.required;
    j["secret"] = f.role == Role::Secret;
    j["role"] = role_to_string(f.role);
    if (f.min || f.max) {
        nlohmann::json range = nlohmann::json::object();
        if (f.kind == FieldRef::Kind::Int) {
            if (f.min) range["min"] = int_bound(*f.min);
            if (f.max) range["max"] = int_bound(*f.max);
        } else {
            if (f.min) range["min"] = *f.min;
            if (f.max) range["max"] = *f.max;
        }
        j["range"] = std::move(range);
    }
    if (!f.allowed_values.empty()) {
        nlohmann::json values = nlohmann::json::array();
        for (const char* v : f.allowed_values) values.push_back(std::string(v));
        j["enum"] = std::move(values);
    }
    if (f.applies_when) {
        j["appliesWhen"] = {{"key", f.applies_when->discriminator_key}, {"value", f.applies_when->value}};
    }
    if (f.kind == FieldRef::Kind::RecordList) {
        nlohmann::json nested = nlohmann::json::array();
        for (const FieldRef& rf : f.record_fields) nested.push_back(field_to_json(rf));
        j["fields"] = std::move(nested);
    }
    return j;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

}  // namespace

alpacacore::catalog::DeviceConfig config_from_json(const nlohmann::json& json,
                                                   std::span<const alpacacore::catalog::FieldRef> fields) {
    return config_from_json_fields(json, fields, "");
}

alpacacore::catalog::DeviceConfig config_from_json(const nlohmann::json& json,
                                                   const alpacacore::catalog::Schema& schema) {
    return config_from_json(json, schema.fields);
}

nlohmann::json json_from_config(const alpacacore::catalog::DeviceConfig& config) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [key, value] : config.entries()) {
        out[key] = value_to_json(value);
    }
    return out;
}

nlohmann::json describe_json(const alpacacore::catalog::DeviceCatalog& catalog) {
    auto views = catalog.describe();
    std::sort(views.begin(), views.end(), [](const auto& a, const auto& b) {
        if (a.key.vendor != b.key.vendor) return a.key.vendor < b.key.vendor;
        return to_lower(alpacacore::device_type_to_string(a.key.type)) <
               to_lower(alpacacore::device_type_to_string(b.key.type));
    });

    nlohmann::json out = nlohmann::json::array();
    for (const auto& view : views) {
        nlohmann::json descriptor;
        descriptor["vendor"] = view.key.vendor;
        descriptor["deviceType"] = to_lower(alpacacore::device_type_to_string(view.key.type));
        descriptor["displayName"] = std::string(view.display_name);
        descriptor["available"] = view.available;
        descriptor["buildOption"] = std::string(view.build_option);
        nlohmann::json fields = nlohmann::json::array();
        for (const auto& f : view.fields) fields.push_back(field_to_json(f));
        descriptor["fields"] = std::move(fields);
        out.push_back(std::move(descriptor));
    }
    return out;
}

}  // namespace alpacahttp::catalog_json

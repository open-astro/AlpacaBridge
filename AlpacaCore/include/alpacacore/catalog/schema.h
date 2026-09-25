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

#pragma once

// Descriptor types of the device catalog (#650). No JSON, no vendor header.
//
// Source policy (#380, PR #353): Source::Api rejects, Source::Persisted warns
// and normalizes so the device still registers and stays editable. This matches
// Router::reject_invalid_config() and normalize_persisted_connection_type().
// Persisted normalization per failure class: missing required keeps the value
// absent; out-of-enum substitutes the field default; out-of-range drops the
// value to unset.
//
// applies_when is honoured by the UI only. normalize and sanitize ignore it.
//
// FieldRef exists because Schema::fields must be iterable without knowing T,
// and Field<T> as first written had no members for the enum and range rules
// that normalize applies (allowed_values, min, max).

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/alpacadriver.h>
#include <alpacacore/catalog/device_config.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace alpacacore::catalog {

enum class Source { Api, Persisted };
enum class Role { Plain, EnumerationIndex, DeviceId, PortPath, Host, Secret, Discriminator };

struct DeviceKey {
    std::string vendor;
    DeviceType type;
    bool operator==(const DeviceKey& o) const { return vendor == o.vendor && type == o.type; }
};

struct AppliesWhen {
    const char* discriminator_key;
    const char* value;
};

// Type-erased field description.
struct FieldRef {
    enum class Kind { Bool, Int, Double, String, StringList, RecordList };
    const char* key = "";
    Kind kind = Kind::String;
    Role role = Role::Plain;
    bool required = false;
    std::optional<AppliesWhen> applies_when;
    std::span<const FieldRef> record_fields;      // RecordList only
    std::span<const char* const> allowed_values;  // enum; empty means unconstrained
    std::optional<double> min;
    std::optional<double> max;
    ConfigValue default_value;
};

template <class T>
struct Field {
    const char* key;
    T default_value;
    bool required = false;
    Role role = Role::Plain;
    std::optional<AppliesWhen> applies_when;
    std::span<const FieldRef> record_fields;      // vector<DeviceConfig> only
    std::span<const char* const> allowed_values;  // string enums
    std::optional<T> min;
    std::optional<T> max;

    FieldRef ref() const {
        FieldRef r;
        r.key = key;
        r.role = role;
        r.required = required;
        r.applies_when = applies_when;
        r.record_fields = record_fields;
        r.allowed_values = allowed_values;
        r.default_value = ConfigValue{default_value};
        if constexpr (std::is_same_v<T, bool>) {
            r.kind = FieldRef::Kind::Bool;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            r.kind = FieldRef::Kind::Int;
        } else if constexpr (std::is_same_v<T, double>) {
            r.kind = FieldRef::Kind::Double;
        } else if constexpr (std::is_same_v<T, std::string>) {
            r.kind = FieldRef::Kind::String;
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            r.kind = FieldRef::Kind::StringList;
        } else {
            static_assert(std::is_same_v<T, std::vector<DeviceConfig>>,
                          "Field<T>: T must be a ConfigValue alternative");
            r.kind = FieldRef::Kind::RecordList;
        }
        if constexpr (std::is_arithmetic_v<T>) {
            if (min) r.min = static_cast<double>(*min);
            if (max) r.max = static_cast<double>(*max);
        }
        return r;
    }
};

template <class T>
std::optional<T> DeviceConfig::find(const Field<T>& field) const {
    const ConfigValue* v = find_value(field.key);
    if (!v) return std::nullopt;
    if (const T* t = std::get_if<T>(v)) return *t;
    return std::nullopt;
}

template <class T>
T DeviceConfig::get(const Field<T>& field) const {
    auto v = find(field);
    return v ? *v : field.default_value;
}

struct NormalizeResult {
    DeviceConfig config;
    std::vector<std::string> warnings;
    std::optional<std::string> rejection;
};

struct Schema {
    DeviceKey key;
    std::string_view display_name;
    std::string_view build_option;
    std::span<const FieldRef> fields;
    // Cross-field rules only. Optional. Must return the FULL config in
    // NormalizeResult::config (copy the input, then adjust); the result replaces
    // the config on success. A rejection's config is ignored. A NaN in a ranged
    // numeric field is out of range, like read_site_coordinates().
    // Lifetime: display_name, build_option, fields and everything FieldRef points
    // at are borrowed, not copied. They must be static or outlive the catalog.
    std::function<NormalizeResult(const DeviceConfig&, Source)> normalize;
};

struct Factory {
    DeviceKey key;
    std::function<std::unique_ptr<AlpacaDriver>(const DeviceConfig&, int device_number)> create;
};

}  // namespace alpacacore::catalog

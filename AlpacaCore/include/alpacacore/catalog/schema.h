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

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace alpacacore::catalog {

enum class Source : std::uint8_t { Api, Persisted };
enum class Role : std::uint8_t { Plain, EnumerationIndex, DeviceId, PortPath, Host, Secret, Discriminator };

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
    enum class Kind : std::uint8_t { Bool, Int, Double, String, StringList, RecordList };
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

// An Int field's bound as the int64 it was declared as. Field::ref() widens
// INT64_MAX to the double 2^63, which no int64 holds and which a plain cast
// turns into undefined behavior, so the bound is clamped to the int64 range.
inline std::int64_t int_bound(double bound) {
    if (bound >= 9223372036854775808.0) return std::numeric_limits<std::int64_t>::max();
    if (bound <= -9223372036854775808.0) return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(bound);
}

template <class T>
struct Field {
    const char* key = "";
    T default_value{};
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
            // Bounds are intentionally double-valued: exact for every plausible device-config bound.
            // An int64 bound beyond 2^53 would compare equal to its neighbours; do not widen this to int64.
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
    // The vendor as user-facing text names it ("Player One"), e.g. in the router's
    // "<label> support not enabled" refusal. Optional: empty means the first word
    // of display_name, which is right for every vendor whose name is one word.
    std::string_view vendor_label;
    // Cross-field rules only. Optional. Must return the FULL config in
    // NormalizeResult::config (copy the input, then adjust); the result replaces
    // the config on success. A rejection's config is ignored. A NaN in a ranged
    // numeric field is out of range, like read_site_coordinates().
    // Lifetime: display_name, build_option, vendor_label, fields and everything FieldRef points
    // at are borrowed, not copied. They must be static or outlive the catalog.
    std::function<NormalizeResult(const DeviceConfig&, Source)> normalize;
    // A persisted selector (a Role::EnumerationIndex or Role::DeviceId field) that
    // cannot be used -- not an integer, or one normalize dropped -- is dropped, and
    // forgotten in the stored entry through the registration's learned config, so
    // the entry still registers and the next start does not warn about it again.
    // Off by default: another vendor's stored entry keeps the key it has.
    bool forget_dropped_selectors = false;
};

// Another persisted entry with the same DeviceKey, as the router holds it.
struct PeerEntry {
    int device_number;
    DeviceConfig config;
};

// What bind() is told about the registration it is part of. `peers` is every
// persisted entry with the same DeviceKey, the one being registered included
// (its device_number is the bound one). Valid for the call only.
struct BindContext {
    Source source;
    std::span<const PeerEntry> peers;
};

// The outcome of Factory::bind. A `rejection` means no device: the router
// reports it (Source::Api) or skips the entry, and the other members are
// ignored. `learned` holds config keys the entry should store (written over the
// stored entry); `erased` names keys to remove from it.
struct BindResult {
    std::unique_ptr<AlpacaDriver> driver;
    std::optional<std::string> rejection;
    DeviceConfig learned;
    std::vector<std::string> erased;
};

// `create` builds a driver from one normalized config and touches no hardware.
// `bind` is optional, for a device whose registration depends on the other
// persisted entries or learns something about its device: it runs instead of
// `create` (DeviceCatalog::bind), may reach the SDK to do so, and is the only
// way to return learned config. A factory with neither is not a factory.
struct Factory {
    DeviceKey key;
    std::function<std::unique_ptr<AlpacaDriver>(const DeviceConfig&, int device_number)> create;
    std::function<BindResult(const DeviceConfig&, int device_number, const BindContext&)> bind;
};

}  // namespace alpacacore::catalog

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

#include <alpacacore/catalog/device_catalog.h>

#include <stdexcept>
#include <string>

namespace alpacacore::catalog {

namespace {

bool matches_kind(FieldRef::Kind kind, const ConfigValue& v) {
    switch (kind) {
        case FieldRef::Kind::Bool: return std::holds_alternative<bool>(v);
        case FieldRef::Kind::Int: return std::holds_alternative<std::int64_t>(v);
        case FieldRef::Kind::Double: return std::holds_alternative<double>(v);
        case FieldRef::Kind::String: return std::holds_alternative<std::string>(v);
        case FieldRef::Kind::StringList: return std::holds_alternative<std::vector<std::string>>(v);
        case FieldRef::Kind::RecordList: return std::holds_alternative<std::vector<DeviceConfig>>(v);
    }
    return false;
}

std::string allowed_list(const FieldRef& f) {
    std::string s;
    for (const char* a : f.allowed_values) {
        if (!s.empty()) s += ", ";
        s += a;
    }
    return s;
}

// Applies the per-field rules to `in`, appending one message per failure.
// The returned config is the Persisted normalization; Api callers use only
// the messages.
DeviceConfig normalize_fields(std::span<const FieldRef> fields,
                              const DeviceConfig& in,
                              const std::string& prefix,
                              std::vector<std::string>& messages) {
    DeviceConfig out = in;
    for (const FieldRef& f : fields) {
        const std::string name = prefix + f.key;
        const ConfigValue* v = in.find_value(f.key);
        if (!v) {
            if (f.required) messages.push_back(name + " is required");
            continue;  // Persisted: stays absent
        }
        if (!matches_kind(f.kind, *v)) {
            messages.push_back(name + " has the wrong type");
            out.erase(f.key);
            continue;
        }
        if (f.kind == FieldRef::Kind::String && !f.allowed_values.empty()) {
            const std::string& s = std::get<std::string>(*v);
            bool ok = false;
            for (const char* a : f.allowed_values) ok = ok || s == a;
            if (!ok) {
                messages.push_back(name + " must be one of: " + allowed_list(f));
                out.set(f.key, f.default_value);
            }
        } else if (f.kind == FieldRef::Kind::Int || f.kind == FieldRef::Kind::Double) {
            const double d = f.kind == FieldRef::Kind::Int
                                 ? static_cast<double>(std::get<std::int64_t>(*v))
                                 : std::get<double>(*v);
            if ((f.min && d < *f.min) || (f.max && d > *f.max)) {
                std::string range = "out of range";
                if (f.min) range += " (min " + std::to_string(*f.min) + ")";
                if (f.max) range += " (max " + std::to_string(*f.max) + ")";
                messages.push_back(name + " is " + range);
                out.erase(f.key);  // Persisted: dropped to unset
            }
        } else if (f.kind == FieldRef::Kind::RecordList) {
            std::vector<DeviceConfig> records = std::get<std::vector<DeviceConfig>>(*v);
            for (std::size_t i = 0; i < records.size(); ++i) {
                records[i] = normalize_fields(f.record_fields, records[i],
                                              name + "[" + std::to_string(i) + "].", messages);
            }
            out.set(f.key, std::move(records));
        }
    }
    return out;
}

DeviceConfig sanitize_fields(std::span<const FieldRef> fields, const DeviceConfig& in) {
    DeviceConfig out;
    for (const FieldRef& f : fields) {
        if (f.role == Role::Secret) continue;
        const ConfigValue* v = in.find_value(f.key);
        if (!v) continue;
        if (f.kind == FieldRef::Kind::RecordList) {
            if (const auto* records = std::get_if<std::vector<DeviceConfig>>(v)) {
                std::vector<DeviceConfig> kept;
                kept.reserve(records->size());
                for (const DeviceConfig& r : *records) kept.push_back(sanitize_fields(f.record_fields, r));
                out.set(f.key, std::move(kept));
                continue;
            }
        }
        out.set(f.key, *v);
    }
    return out;
}

std::string describe_key(const DeviceKey& k) {
    return "'" + k.vendor + "' " + device_type_to_string(k.type);
}

}  // namespace

std::vector<DescriptorView> DeviceCatalog::describe() const {
    std::vector<DescriptorView> views;
    views.reserve(schemas_.size());
    for (const Schema& s : schemas_) {
        views.push_back({s.key, s.display_name, s.build_option, find_factory(s.key) != nullptr, s.fields});
    }
    return views;
}

NormalizeResult DeviceCatalog::normalize(const DeviceKey& key,
                                         const DeviceConfig& in,
                                         Source source) const {
    NormalizeResult result;
    result.config = in;
    const Schema* schema = find_schema(key);
    if (!schema) {
        std::string msg = "unknown device " + describe_key(key);
        if (source == Source::Api) result.rejection = msg;
        else result.warnings.push_back(msg);
        return result;
    }

    std::vector<std::string> messages;
    DeviceConfig normalized = normalize_fields(schema->fields, in, "", messages);
    if (source == Source::Api) {
        if (!messages.empty()) {
            result.rejection = messages.front();
            return result;
        }
    } else {
        result.warnings = std::move(messages);
        result.config = std::move(normalized);
    }

    if (schema->normalize) {
        NormalizeResult cross = schema->normalize(source == Source::Api ? in : result.config, source);
        if (cross.rejection) {
            if (source == Source::Api) {
                result.rejection = std::move(cross.rejection);
                return result;
            }
            cross.warnings.push_back(*cross.rejection);
        }
        for (auto& w : cross.warnings) result.warnings.push_back(std::move(w));
        result.config = std::move(cross.config);
    }
    return result;
}

DeviceConfig DeviceCatalog::sanitize(const DeviceKey& key, const DeviceConfig& in) const {
    const Schema* schema = find_schema(key);
    return schema ? sanitize_fields(schema->fields, in) : DeviceConfig{};
}

std::unique_ptr<AlpacaDriver> DeviceCatalog::create(const DeviceKey& key,
                                                    const DeviceConfig& normalized,
                                                    int device_number) const {
    const Schema* schema = find_schema(key);
    if (!schema) {
        throw std::runtime_error("cannot create unknown device " + describe_key(key));
    }
    const Factory* factory = find_factory(key);
    if (!factory || !factory->create) {
        throw std::runtime_error("device " + describe_key(key) + " is not available in this build (built without " +
                                 std::string(schema->build_option) + ")");
    }
    return factory->create(normalized, device_number);
}

void DeviceCatalog::add(Schema schema) {
    for (Schema& s : schemas_) {
        if (s.key == schema.key) {
            s = std::move(schema);
            return;
        }
    }
    schemas_.push_back(std::move(schema));
}

void DeviceCatalog::add(Factory factory) {
    for (Factory& f : factories_) {
        if (f.key == factory.key) {
            f = std::move(factory);
            return;
        }
    }
    factories_.push_back(std::move(factory));
}

const Schema* DeviceCatalog::find_schema(const DeviceKey& key) const {
    for (const Schema& s : schemas_) {
        if (s.key == key) return &s;
    }
    return nullptr;
}

const Factory* DeviceCatalog::find_factory(const DeviceKey& key) const {
    for (const Factory& f : factories_) {
        if (f.key == key) return &f;
    }
    return nullptr;
}

void register_builtin_schemas(DeviceCatalog&) {}
void register_builtin_factories(DeviceCatalog&) {}

}  // namespace alpacacore::catalog

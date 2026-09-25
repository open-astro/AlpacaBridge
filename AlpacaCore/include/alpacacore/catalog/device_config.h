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

// Typed, JSON-free device configuration (device catalog, #650).
//
// The #388 null rule: a JSON null means "unset", not a value. DeviceConfig
// stores an entry only for keys that were set, so an unset key (has() false,
// find() nullopt) is distinguishable from a key set to an empty record list
// (has() true, find() an empty vector). A later JSON bridge maps null to
// "no entry" and has nothing to invent. This holds inside nested record lists
// too, because every record is itself a DeviceConfig.
//
// get()/find() are templates over Field<T> and are defined in schema.h.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace alpacacore::catalog {

template <class T>
struct Field;

class DeviceConfig;

using ConfigValue = std::variant<bool,
                                 std::int64_t,
                                 double,
                                 std::string,
                                 std::vector<std::string>,   // filterNames
                                 std::vector<DeviceConfig>>; // ports[]: one nested config per record

class DeviceConfig {
public:
    DeviceConfig();
    ~DeviceConfig();
    DeviceConfig(const DeviceConfig&);
    DeviceConfig(DeviceConfig&&) noexcept;
    DeviceConfig& operator=(const DeviceConfig&);
    DeviceConfig& operator=(DeviceConfig&&) noexcept;

    // Value stored for the field, or its default_value when unset or of a different type.
    template <class T>
    T get(const Field<T>& field) const;
    // Value stored for the field, or nullopt when unset or of a different type.
    template <class T>
    std::optional<T> find(const Field<T>& field) const;

    void set(std::string_view key, ConfigValue value);
    bool has(std::string_view key) const;
    void erase(std::string_view key);

    const ConfigValue* find_value(std::string_view key) const;
    const std::vector<std::pair<std::string, ConfigValue>>& entries() const { return entries_; }

private:
    std::vector<std::pair<std::string, ConfigValue>> entries_;
};

}  // namespace alpacacore::catalog

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

#include <alpacacore/catalog/device_config.h>
#include <alpacacore/catalog/schema.h>

#include <algorithm>

namespace alpacacore::catalog {

DeviceConfig::DeviceConfig() = default;
DeviceConfig::~DeviceConfig() = default;
DeviceConfig::DeviceConfig(const DeviceConfig&) = default;
DeviceConfig::DeviceConfig(DeviceConfig&&) noexcept = default;
DeviceConfig& DeviceConfig::operator=(const DeviceConfig&) = default;
DeviceConfig& DeviceConfig::operator=(DeviceConfig&&) noexcept = default;

void DeviceConfig::set(std::string_view key, ConfigValue value) {
    for (auto& e : entries_) {
        if (e.first == key) {
            e.second = std::move(value);
            return;
        }
    }
    entries_.emplace_back(std::string(key), std::move(value));
}

bool DeviceConfig::has(std::string_view key) const { return find_value(key) != nullptr; }

void DeviceConfig::erase(std::string_view key) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const auto& e) { return e.first == key; }),
                   entries_.end());
}

const ConfigValue* DeviceConfig::find_value(std::string_view key) const {
    for (const auto& e : entries_) {
        if (e.first == key) return &e.second;
    }
    return nullptr;
}

}  // namespace alpacacore::catalog

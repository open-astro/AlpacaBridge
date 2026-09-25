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

namespace alpacacore::catalog {

std::vector<DescriptorView> DeviceCatalog::describe() const { return {}; }

NormalizeResult DeviceCatalog::normalize(const DeviceKey&, const DeviceConfig& in, Source) const {
    NormalizeResult r;
    r.config = in;
    return r;
}

DeviceConfig DeviceCatalog::sanitize(const DeviceKey&, const DeviceConfig& in) const { return in; }

std::unique_ptr<AlpacaDriver> DeviceCatalog::create(const DeviceKey&,
                                                    const DeviceConfig&,
                                                    int) const {
    return nullptr;
}

void DeviceCatalog::add(Schema schema) { schemas_.push_back(std::move(schema)); }
void DeviceCatalog::add(Factory factory) { factories_.push_back(std::move(factory)); }

const Schema* DeviceCatalog::find_schema(const DeviceKey&) const { return nullptr; }
const Factory* DeviceCatalog::find_factory(const DeviceKey&) const { return nullptr; }

void register_builtin_schemas(DeviceCatalog&) {}
void register_builtin_factories(DeviceCatalog&) {}

}  // namespace alpacacore::catalog

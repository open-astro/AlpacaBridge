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

// DeviceCatalog (#650): describe, normalize, sanitize, create.
// No process-wide singleton: vendors and test doubles add descriptors to an
// instance. Add/lookup shape follows management::DeviceRegistry.

#include <alpacacore/catalog/schema.h>

#include <memory>
#include <string_view>
#include <vector>

namespace alpacacore::catalog {

struct DescriptorView {
    DeviceKey key;
    std::string_view display_name;
    std::string_view build_option;
    bool available;
    std::span<const FieldRef> fields;
};

class DeviceCatalog {
public:
    // Every key with a schema, in insertion order. available means "a factory is
    // registered for this key", so a build with a vendor off still names it.
    std::vector<DescriptorView> describe() const;

    // Per-field rules (required, default, enum, range; record lists recurse and
    // messages carry the index, e.g. "ports[1].name is required"), then the
    // schema's cross-field normalize. Source::Api: the first failure becomes
    // NormalizeResult::rejection and the config is returned as given; on success
    // the normalized config is returned.
    // Source::Persisted: every failure becomes a warning and the config is
    // normalized so the device still registers (missing required stays absent,
    // out-of-enum becomes the default, out-of-range becomes unset). A wrong-type
    // value is a failure too (Api rejects, Persisted erases it). An int64 in a Double
    // field is accepted and widened to double before the range check.
    //
    // Callers pass the raw input config to sanitize() and register the normalized
    // config, so a persisted raw value that normalize changed or dropped stays
    // visible in configureddevices.
    NormalizeResult normalize(const DeviceKey& key, const DeviceConfig& in, Source source) const;

    // Every declared field except Role::Secret, recursing into record lists.
    // applies_when is ignored: a field whose discriminator says it does not apply
    // is kept, because the persisted file keeps it too. Undeclared keys are dropped.
    DeviceConfig sanitize(const DeviceKey& key, const DeviceConfig& in) const;

    // Requires a factory. Throws std::runtime_error naming Schema::build_option
    // when the key has a schema but no factory, or naming the key when unknown.
    std::unique_ptr<AlpacaDriver> create(const DeviceKey& key, const DeviceConfig& normalized, int device_number) const;

    void add(Schema schema);    // replaces a schema with the same key
    void add(Factory factory);  // replaces a factory with the same key

private:
    const Schema* find_schema(const DeviceKey& key) const;
    const Factory* find_factory(const DeviceKey& key) const;
    std::vector<Schema> schemas_;
    std::vector<Factory> factories_;
};

// Empty until the vendor-descriptor slices land (see docs/decisions/0004-device-catalog.md).
void register_builtin_schemas(DeviceCatalog& catalog);
void register_builtin_factories(DeviceCatalog& catalog);

}  // namespace alpacacore::catalog

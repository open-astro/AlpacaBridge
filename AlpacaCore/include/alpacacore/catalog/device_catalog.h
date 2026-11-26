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
#include <alpacacore/util/thermal_camera_choice.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alpacacore::catalog {

struct DescriptorView {
    DeviceKey key;
    std::string_view display_name;
    std::string_view build_option;
    bool available;
    std::span<const FieldRef> fields;
    std::string_view vendor_label;  // Schema::vendor_label, empty when the schema sets none
    bool forget_dropped_selectors;  // Schema::forget_dropped_selectors
};

// One attached camera as a vendor's camera lister reports it.
struct AttachedCamera {
    int index{};         // the cameraIndex value that selects this camera
    std::string name;    // model name, trimmed
    std::string serial;  // empty when the camera reports none or it was not read
};

// A USB-serial adapter a catalog entry can appear as (#2312), for the
// attacheddevices listing. `tokens` are usb_tty_descriptor_matches() patterns
// (a vendor id, or a manufacturer, product or by-id name substring). The
// vendor's catalog file passes the list its raw-node scan filter uses, from
// the wrapper's <x>_usb_tokens(), so the listing and the connect-time scan
// cannot disagree (docs-drift check 29). `config` holds the fixed values a
// configuredevice call needs beyond the port (QHY CFW3: wheelType=cfw3-usb).
struct UsbSerialIdentity {
    DeviceKey key;
    std::vector<std::string> tokens;
    std::vector<std::pair<std::string, std::string>> config;
};

// One HID device a vendor's HID lister found by enumeration, without opening it.
struct AttachedHidDevice {
    std::string path;  // the hidapi path a configuredevice call names
    std::string name;  // display name
};

// One network adapter a vendor's finder heard announce itself on the LAN (the SkyPortal WiFi adapter's beacon).
struct FoundNetworkAdapter {
    std::string host;    // the numeric address a configuredevice call names
    std::string ssid;    // the network the adapter serves, empty when it did not say
    std::string module;  // the adapter's radio module, empty when it did not say
};

class DeviceCatalog {
public:
    // Every key with a schema, in insertion order. available means "a factory is
    // registered for this key", so a build with a vendor off still names it.
    std::vector<DescriptorView> describe() const;
    // The view of one key, or nullopt when the key has no schema.
    std::optional<DescriptorView> describe(const DeviceKey& key) const;

    // Every key with a factory, in insertion order, schema or not: a factory whose
    // schema was never added is unreachable, and describe() cannot show it.
    // For diagnostics and tests (the contract sweep reads it); it is not a
    // runtime availability API, so a caller must not ask it whether a device
    // can be created. Use describe() for what the server offers.
    std::vector<DeviceKey> factory_keys() const;

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
    // A factory that has only `bind` cannot be created without its context: it throws
    // std::runtime_error saying so.
    std::unique_ptr<AlpacaDriver> create(const DeviceKey& key, const DeviceConfig& normalized, int device_number) const;

    // True when the key's factory has a `bind` hook, so the caller must gather the
    // BindContext peers before bind().
    bool has_bind(const DeviceKey& key) const;

    // Factory::bind when set, else create() wrapped in a BindResult with nothing
    // learned. Same errors as create() when the key is unknown or has no factory.
    BindResult bind(const DeviceKey& key, const DeviceConfig& normalized, int device_number,
                    const BindContext& context) const;

    void add(Schema schema);    // replaces a schema with the same key
    void add(Factory factory);  // replaces a factory with the same key

    // The attached cameras a vendor's camera-attached thermal switch (dew heater,
    // fan) can bind to, for the web UI's camera picker. A lister talks to the
    // vendor SDK, may open cameras through the vendor's ref-counted wrapper, and
    // may throw. Registered only when the vendor is built.
    using ThermalCameraLister = std::function<std::vector<util::ThermalCameraCandidate>()>;
    void add_thermal_camera_lister(std::string vendor, ThermalCameraLister lister);  // replaces the vendor's
    // nullopt when no lister is registered for the vendor; otherwise the lister's result.
    std::optional<std::vector<util::ThermalCameraCandidate>> list_thermal_cameras(std::string_view vendor) const;

    // The attached cameras a vendor's camera entry can bind to, for the web UI's
    // camera picker (#1970). Same contract as a thermal lister: talks to the SDK,
    // may open cameras, may throw, registered only when the vendor is built.
    using CameraLister = std::function<std::vector<AttachedCamera>()>;
    void add_camera_lister(std::string vendor, CameraLister lister);  // replaces the vendor's
    // nullopt when no lister is registered for the vendor; otherwise the lister's result.
    std::optional<std::vector<AttachedCamera>> list_cameras(std::string_view vendor) const;
    // The vendors with a camera lister, in registration order (attacheddevices, #2312).
    std::vector<std::string> camera_lister_vendors() const;

    // The attacheddevices listing (#2312). A USB identity is registered by the
    // vendor's factory file, so only a built vendor's devices are listed; an
    // identity for a key that is already registered adds its tokens as another
    // identity, never replaces it. A HID lister enumerates without opening a
    // device and may throw; registered only when the vendor is built.
    void add_usb_serial_identity(UsbSerialIdentity identity);
    const std::vector<UsbSerialIdentity>& usb_serial_identities() const { return usb_serial_identities_; }
    using HidLister = std::function<std::vector<AttachedHidDevice>()>;
    void add_hid_lister(DeviceKey key, HidLister lister);  // replaces the key's
    const std::vector<std::pair<DeviceKey, HidLister>>& hid_listers() const { return hid_listers_; }

    // The adapters a vendor's network devices announce on the LAN, for the web UI's "Find adapter" button. A
    // finder listens for a bounded window (it blocks the caller for all of it), sends nothing and may throw;
    // registered only when the vendor is built.
    using NetworkAdapterFinder = std::function<std::vector<FoundNetworkAdapter>()>;
    void add_network_adapter_finder(std::string vendor, NetworkAdapterFinder finder);  // replaces the vendor's
    bool has_network_adapter_finder(std::string_view vendor) const;
    // nullopt when no finder is registered for the vendor; otherwise the finder's result.
    std::optional<std::vector<FoundNetworkAdapter>> find_network_adapters(std::string_view vendor) const;

private:
    const Schema* find_schema(const DeviceKey& key) const;
    const Factory* find_factory(const DeviceKey& key) const;
    std::vector<Schema> schemas_;
    std::vector<Factory> factories_;
    std::vector<std::pair<std::string, ThermalCameraLister>> thermal_camera_listers_;
    std::vector<std::pair<std::string, CameraLister>> camera_listers_;
    std::vector<UsbSerialIdentity> usb_serial_identities_;
    std::vector<std::pair<DeviceKey, HidLister>> hid_listers_;
    std::vector<std::pair<std::string, NetworkAdapterFinder>> network_adapter_finders_;
};

// Formats a range bound for a message: an Int field without a fraction, a
// Double field in its shortest round-trip form (0.5, 100).
std::string format_bound(FieldRef::Kind kind, double bound);

}  // namespace alpacacore::catalog

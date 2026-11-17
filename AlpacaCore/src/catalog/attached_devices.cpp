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

#include <alpacacore/catalog/attached_devices.h>
#include <alpacacore/util/serial_by_id_scan.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace alpacacore::catalog {

namespace {

namespace fs = std::filesystem;

std::string canonical_or_empty(const fs::path& path) {
    std::error_code ec;
    auto resolved = fs::canonical(path, ec);
    return ec ? std::string() : resolved.string();
}

// ttyUSB<digits> or ttyACM<digits>: the raw nodes every serial wrapper's
// fallback scan reads.
bool is_raw_tty_name(std::string_view name) {
    for (std::string_view prefix : {std::string_view("ttyUSB"), std::string_view("ttyACM")}) {
        if (name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix &&
            std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
            return true;
        }
    }
    return false;
}

// The raw nodes in `dev`, by name. error_code overloads only, so an unplug
// mid-scan ends the scan with what it found.
std::vector<fs::path> raw_tty_nodes(const fs::path& dev) {
    std::vector<fs::path> nodes;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dev, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (is_raw_tty_name(it->path().filename().string())) nodes.push_back(it->path());
    }
    std::sort(nodes.begin(), nodes.end());
    return nodes;
}

// A by-id name carries udev's spelling of the descriptor strings, so a token
// is a substring match, as each wrapper's by-id filter reads it.
bool by_id_name_matches(const std::string& name, const std::vector<std::string>& tokens) {
    return std::any_of(tokens.begin(), tokens.end(),
                       [&name](const std::string& token) { return name.find(token) != std::string::npos; });
}

// By value: a FieldRef is small, and a pointer into the borrowed span reads to
// cppcheck as a pointer to a local.
std::optional<FieldRef> first_field(std::span<const FieldRef> fields, Role role) {
    for (const FieldRef& field : fields) {
        if (field.role == role) return field;
    }
    return std::nullopt;
}

std::optional<FieldRef> field_named(std::span<const FieldRef> fields, std::string_view key) {
    for (const FieldRef& field : fields) {
        if (key == field.key) return field;
    }
    return std::nullopt;
}

// A string field's value in `config`, else its declared default.
std::string string_value(const DeviceConfig& config, std::span<const FieldRef> fields, std::string_view key) {
    if (const ConfigValue* value = config.find_value(key)) {
        if (const auto* text = std::get_if<std::string>(value)) return *text;
        return {};
    }
    if (const auto field = field_named(fields, key)) {
        if (const auto* text = std::get_if<std::string>(&field->default_value)) return *text;
    }
    return {};
}

// The candidate for `key` with the port under its descriptor's PortPath field
// and that field's discriminator set, or nullopt when the key has no schema or
// no port field.
std::optional<AttachedCandidate> port_candidate(const DeviceCatalog& catalog, const DeviceKey& key,
                                                const std::string& port,
                                                const std::vector<std::pair<std::string, std::string>>& fixed) {
    const auto view = catalog.describe(key);
    if (!view) return std::nullopt;
    const auto port_field = first_field(view->fields, Role::PortPath);
    if (!port_field) return std::nullopt;
    AttachedCandidate candidate{key, {}};
    for (const auto& [name, value] : fixed) candidate.config.set(name, value);
    if (port_field->applies_when) {
        candidate.config.set(port_field->applies_when->discriminator_key, std::string(port_field->applies_when->value));
    }
    candidate.config.set(port_field->key, port);
    return candidate;
}

// The configured devices that name this port or HID path in a port field that
// applies to their config.
std::vector<ConfiguredDeviceRef> configured_on_port(const DeviceCatalog& catalog,
                                                    const std::vector<ConfiguredDevice>& configured,
                                                    const std::string& port, const std::string& node) {
    std::vector<ConfiguredDeviceRef> refs;
    for (const ConfiguredDevice& device : configured) {
        const auto found = catalog.describe(device.key);
        if (!found) continue;
        const DescriptorView& view = *found;
        for (const FieldRef& field : view.fields) {
            if (field.role != Role::PortPath) continue;
            if (const auto& when = field.applies_when;
                when && string_value(device.config, view.fields, when->discriminator_key) != when->value) {
                continue;
            }
            const std::string value = string_value(device.config, view.fields, field.key);
            if (value.empty()) continue;
            const bool same = value == port || (!node.empty() && (value == node || canonical_or_empty(value) == node));
            if (same) {
                refs.push_back({device.key.type, device.device_number});
                break;
            }
        }
    }
    return refs;
}

void add_serial_port(const DeviceCatalog& catalog, const std::vector<ConfiguredDevice>& configured,
                     AttachedDeviceList& list, const std::string& port, const std::string& node,
                     const std::string& by_id_name, const std::optional<util::UsbTtyDescriptor>& descriptor) {
    AttachedDevice device;
    device.kind = AttachedKind::Serial;
    device.port = port;
    device.node = node;
    for (const UsbSerialIdentity& identity : catalog.usb_serial_identities()) {
        const bool matches = (descriptor && util::usb_tty_descriptor_matches(*descriptor, identity.tokens)) ||
                             (!by_id_name.empty() && by_id_name_matches(by_id_name, identity.tokens));
        if (!matches) continue;
        const bool listed = std::any_of(device.candidates.begin(), device.candidates.end(),
                                        [&identity](const AttachedCandidate& c) { return c.key == identity.key; });
        if (listed) continue;
        if (auto candidate = port_candidate(catalog, identity.key, port, identity.config)) {
            device.candidates.push_back(std::move(*candidate));
        }
    }
    if (device.candidates.empty()) return;
    if (!by_id_name.empty()) {
        device.name = by_id_name;
    } else if (descriptor && !(descriptor->manufacturer.empty() && descriptor->product.empty())) {
        device.name = descriptor->manufacturer.empty() ? descriptor->product
                      : descriptor->product.empty()    ? descriptor->manufacturer
                                                       : descriptor->manufacturer + " " + descriptor->product;
    } else {
        device.name = fs::path(node).filename().string();
    }
    device.configured_as = configured_on_port(catalog, configured, port, node);
    list.devices.push_back(std::move(device));
}

void list_serial_ports(const DeviceCatalog& catalog, const std::vector<ConfiguredDevice>& configured,
                       const AttachedDeviceRoots& roots, AttachedDeviceList& list) {
    if (catalog.usb_serial_identities().empty()) return;
    std::set<std::string> seen;
    for (const auto& entry : util::list_serial_by_id(roots.serial_by_id)) {
        if (list.devices.size() >= kMaxAttachedDevices) return;
        const std::string node = canonical_or_empty(entry.path);
        if (node.empty() || !seen.insert(node).second) continue;
        add_serial_port(catalog, configured, list, entry.path.string(), node, entry.name,
                        util::read_raw_tty_usb_descriptor(node, roots.tty_class));
    }
    // A second adapter with the same descriptor strings gets no by-id link
    // (udev's names collide), so the raw nodes are read too, filtered by the
    // sysfs descriptor alone.
    for (const auto& raw : raw_tty_nodes(roots.dev)) {
        if (list.devices.size() >= kMaxAttachedDevices) return;
        const std::string node = canonical_or_empty(raw);
        if (node.empty() || !seen.insert(node).second) continue;
        const auto descriptor = util::read_raw_tty_usb_descriptor(node, roots.tty_class);
        if (!descriptor) continue;
        add_serial_port(catalog, configured, list, node, node, std::string(), descriptor);
    }
}

void list_hid_devices(const DeviceCatalog& catalog, const std::vector<ConfiguredDevice>& configured,
                      AttachedDeviceList& list) {
    for (const auto& [key, lister] : catalog.hid_listers()) {
        if (!lister) continue;
        std::vector<AttachedHidDevice> found;
        try {
            found = lister();
        } catch (const std::exception& e) {
            list.errors.push_back({key.vendor + " HID lister", e.what()});
            continue;
        } catch (...) {
            list.errors.push_back({key.vendor + " HID lister", "unknown error"});
            continue;
        }
        for (const AttachedHidDevice& hid : found) {
            if (list.devices.size() >= kMaxAttachedDevices) return;
            auto candidate = port_candidate(catalog, key, hid.path, {});
            if (!candidate) continue;
            AttachedDevice device;
            device.kind = AttachedKind::Hid;
            device.port = hid.path;
            device.name = hid.name;
            device.candidates.push_back(std::move(*candidate));
            device.configured_as = configured_on_port(catalog, configured, hid.path, std::string());
            list.devices.push_back(std::move(device));
        }
    }
}

// The camera fields set from a listed camera: the descriptor's enumeration
// index field, and the ZWO binding fields (`serialNumber`, `cameraName`, #914)
// when the descriptor declares them.
AttachedCandidate camera_candidate(const DescriptorView& view, const AttachedCamera& camera) {
    AttachedCandidate candidate{view.key, {}};
    if (const auto index = first_field(view.fields, Role::EnumerationIndex)) {
        candidate.config.set(index->key, static_cast<std::int64_t>(camera.index));
    }
    if (!camera.serial.empty() && field_named(view.fields, "serialNumber").has_value()) {
        candidate.config.set("serialNumber", camera.serial);
    }
    if (!camera.name.empty() && field_named(view.fields, "cameraName").has_value()) {
        candidate.config.set("cameraName", camera.name);
    }
    return candidate;
}

void list_cameras(const DeviceCatalog& catalog, const std::vector<ConfiguredDevice>& configured,
                  AttachedDeviceList& list) {
    for (const std::string& vendor : catalog.camera_lister_vendors()) {
        const DeviceKey key{vendor, DeviceType::Camera};
        const auto described = catalog.describe(key);
        if (!described) continue;
        const DescriptorView& view = *described;
        std::optional<std::vector<AttachedCamera>> found;
        try {
            found = catalog.list_cameras(vendor);
        } catch (const std::exception& e) {
            list.errors.push_back({vendor + " camera lister", e.what()});
            continue;
        } catch (...) {
            list.errors.push_back({vendor + " camera lister", "unknown error"});
            continue;
        }
        if (!found) continue;
        for (const AttachedCamera& camera : *found) {
            if (list.devices.size() >= kMaxAttachedDevices) return;
            AttachedDevice device;
            device.kind = AttachedKind::Camera;
            device.serial_number = camera.serial;
            device.camera_index = camera.index;
            device.name = camera.name;
            device.candidates.push_back(camera_candidate(view, camera));
            if (!camera.serial.empty()) {
                for (const ConfiguredDevice& entry : configured) {
                    if (entry.key == key && string_value(entry.config, view.fields, "serialNumber") == camera.serial) {
                        device.configured_as.push_back({entry.key.type, entry.device_number});
                    }
                }
            }
            list.devices.push_back(std::move(device));
        }
    }
}

}  // namespace

AttachedDeviceList list_attached_devices(const DeviceCatalog& catalog, const std::vector<ConfiguredDevice>& configured,
                                         const AttachedDeviceRoots& roots) {
    AttachedDeviceList list;
    list_serial_ports(catalog, configured, roots, list);
    list_hid_devices(catalog, configured, list);
    list_cameras(catalog, configured, list);
    return list;
}

}  // namespace alpacacore::catalog
